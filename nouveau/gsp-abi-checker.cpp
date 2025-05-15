// g++ -I`gcc -print-file-name=plugin`/include -fPIC -shared -o gsp-abi-checker.so gsp-abi-checker.cpp

#include <gcc-plugin.h>
#include <plugin-version.h>
#include <tree.h>
#include <tree-pass.h>
#include <cp/cp-tree.h>
#include <tree-pretty-print.h>
#include <c-family/c-common.h>
#include <cpplib.h>
#include <iostream>
#include <stdio.h>
#include <string>
#include <vector>
#include <unordered_map>
#include <map>
#include <set>
int plugin_is_GPL_compatible;

#define DBG(fmt, ...) //fprintf(stderr, fmt, ##__VA_ARGS__)
#define WARN(fmt, ...) fprintf(stderr, "WARNING: " fmt, ##__VA_ARGS__)
#define ERROR(fmt, ...) fprintf(stderr, "ERROR: " fmt, ##__VA_ARGS__)


// Plugin only runs on this file, otherwise it's a NOP
const char *gsp_abi_file = "gsp_abi_check.c";
static FILE *outfile = stderr;

// TODO: These should be tagged in the source with #pragma or __attribute__
// But for now just hardcode a list...
static std::set<std::string> wanted = {
    #define X_STRUCT(x) #x,
    #define X_MACRO(x) #x,
    #define X_ENUM(x) #x,
    #include "wanted.h"
};
static std::vector<std::string> wanted_prefix = {
    #define X_MACRO_PREFIX(x) #x,
    #include "wanted.h"
};
static bool is_wanted(const char* name) {
    if (wanted.find(name) != wanted.end()) {
        return true;
    }
    for (auto& prefix : wanted_prefix) {
        if (!strncmp(name, prefix.c_str(), prefix.size())) {
            return true;
        }
    }
    return false;
}

// These structures can be extended by appending if needed
static std::set<std::string> flexible = {
    "GspStaticConfigInfo",
    "GspSystemInfo",
    "rpc_alloc_memory_v13_01",
    "rpc_free_v03_00",
    "rpc_gsp_rm_alloc_v03_00",
    "rpc_gsp_rm_control_v03_00",
    "rpc_os_error_log_v17_00",
    "rpc_post_event_v17_00",
    "rpc_rc_triggered_v17_02",
    "rpc_run_cpu_sequencer_v17_00",
    "rpc_unloading_guest_driver_v1F_07",
    "rpc_update_bar_pde_v15_00",
};
static std::set<std::string> extendible_enums = {
    "NV_VGPU_MSG_FUNCTION",
    "NV_VGPU_MSG_EVENT",
};

struct AbiChecked {
    std::string name;
    virtual void print(FILE *f) = 0;
};
std::map<std::string, AbiChecked*> all;

struct Struct : AbiChecked {
    size_t size;
    struct Field {
        std::string name;
        size_t size;
        size_t offset;
    };
    std::vector<Field> fields;

    Struct() = default;
    Struct(tree t) {
        name = IDENTIFIER_POINTER(TYPE_NAME(t));
        size = int_size_in_bytes(t);
        for (tree field = TYPE_FIELDS(t); field; field = TREE_CHAIN(field)) {
            if (!DECL_NAME(field)) {
                continue;
            }
            Field f;
            f.name = IDENTIFIER_POINTER(DECL_NAME(field));
            f.size = int_size_in_bytes(TREE_TYPE(field));
            f.offset = TREE_INT_CST_LOW(byte_position(field));
            fields.push_back(f);
        }
    }

    virtual void print(FILE *f) override {
        fprintf(f, "\n");
        if (flexible.count(name)) {
            fprintf(f, "// Appending to the end of the struct is okay.\n");
            fprintf(f, "ABI_CHECK_SIZE_GE(%s, %ld);\n", name.c_str(), size);
        } else {
            fprintf(f, "ABI_CHECK_SIZE_EQ(%s, %ld);\n", name.c_str(), size);
        }
        for (auto& field : fields) {
            if (field.size == ~0) {
                if (field.name == fields.back().name) {
                    fprintf(f, "ABI_CHECK_FIELD_FLEXIBLE(%s, %s, %ld);\n", name.c_str(), field.name.c_str(), field.offset);
                    continue;
                }
                WARN("Failed to get size for %s.%s\n", name.c_str(), field.name.c_str());
            }
            fprintf(f, "ABI_CHECK_FIELD(%s, %s, %ld, %ld);\n", name.c_str(), field.name.c_str(), field.offset, field.size);
        }
        fprintf(f, "\n");
    }
};

struct Macro : AbiChecked {
    std::string value;

    Macro() = default;
    Macro(const char *def) {
        if (auto val = strchr((const char*)def, ' ')) {
            name = std::string(def, val-def);
            while (*val == ' ')
                val++;
            value = val;
        }
    }
    virtual void print(FILE *f) override {
        // DRF definitions and the like can't be checked with ABI_CHECK_VALUE
        // Simply redefine them. Compiler allows redifinition IFF the definition is the same.
        fprintf(f, "#define %s %s\n", name.c_str(), value.c_str());
    }
};

struct Enum : AbiChecked {
    std::vector<std::pair<std::string, long>> values;

    Enum() = default;
    Enum(tree t) {
        name = IDENTIFIER_POINTER(TYPE_NAME(t));
        for (tree value = TYPE_VALUES(t); value; value = TREE_CHAIN(value)) {
            if (!TREE_VALUE(value)) {
                WARN("No value for field\n");
                continue;
            }
            if (!TREE_CODE(TREE_VALUE(value)) == INTEGER_CST) {
                WARN("Field is not an integer\n");
                continue;
            }

            std::string field_name = IDENTIFIER_POINTER(TREE_PURPOSE(value));
            long field_value = TREE_INT_CST_LOW(TREE_VALUE(value));
            values.push_back({field_name, field_value});
        }
    }

    virtual void print(FILE *f) override {
        fprintf(f, "\n");
        for (auto& field : values) {
            if (extendible_enums.count(name) && field.first == values.back().first) {
                fprintf(f, "// Appending to the end of this enum is okay.\n");
                fprintf(f, "ABI_CHECK_ENUM_VAL_GE(%s, %s, %ld);\n", name.c_str(), field.first.c_str(), field.second);
            } else {
                fprintf(f, "ABI_CHECK_ENUM_VAL_EQ(%s, %s, %ld);\n", name.c_str(), field.first.c_str(), field.second);
            }
        }
        fprintf(f, "\n\n");
    }
};


static void finish_type_callback(void* gcc_data, void* user_data) {
    tree type = (tree)gcc_data;
    if (!type || !TYPE_NAME(type)) {
        return;
    }

    const char* type_name = IDENTIFIER_POINTER(TYPE_NAME(type));
    if (is_wanted(type_name) && !all.count(type_name)) {           
        if (TREE_CODE(type) == RECORD_TYPE) {
            auto* s = new Struct(type);
            if (s->size == ~0) {
                delete s;
                return;
            }
            all[type_name] = s;
        }
        else if (TREE_CODE(type) == ENUMERAL_TYPE) {
            all[type_name] = new Enum(type);
        }
    }
    
}

static void finish_unit_callback(void* gcc_data, void* user_data) {
    extern cpp_reader *parse_in;
    cpp_forall_identifiers(parse_in, [](cpp_reader *reader, cpp_hashnode *node, void *data) {
        if (node && cpp_macro_p(node)) {
            const char *name = (const char *)NODE_NAME(node);
            if (is_wanted(name) && !all.count(name)) {
                all[name] = new Macro((const char*)cpp_macro_definition(reader, node));
            }
        }
        return 1;
    }, NULL);

    for (auto& w : wanted) {
        if (all.find(w) == all.end()) {
            WARN("Missing wanted symbol %s\n", w.c_str());
        }        
    }

    for (auto& s : all) {
        s.second->print(outfile);
    }

    fclose(outfile);
    outfile = stderr;
}

static void write_preamble(FILE *f) {
    const char *preamble = R"preabmle(//
// This file enforces partial GSP ABI stability within a release branch
//
// If you are hitting one of the asserts here, this means your changes end up
// breaking the ABI between the GSP and the CPU in a way that will break other
// kernel drivers such as nouveau.
//
// Please see bug 5095544 for more details.
//
// This file was generated automatically, but may have had further manual
// changes applied to it. Check bug 5095544 and p4 history.
//

#define RPC_STRUCTURES
#include "g_rpc-structures.h"
#include "g_sdk-structures.h"
#include <nvos.h>
#include <alloc/alloc_channel.h>
#include <class/cl0000.h>
#include <class/cl0005.h>
#include <class/cl0073.h>
#include <class/cl0080.h>
#include <class/cl2080.h>
#include <class/cl2080_notification.h>
#include <class/cl84a0.h>
#include <class/cl90f1.h>
#include <class/clc0b5sw.h>
#include <ctrl/ctrl0073/ctrl0073dp.h>
#include <ctrl/ctrl0073/ctrl0073common.h>
#include <ctrl/ctrl0073/ctrl0073system.h>
#include <ctrl/ctrl0073/ctrl0073specific.h>
#include <ctrl/ctrl0073/ctrl0073dfp.h>
#include <ctrl/ctrl0080/ctrl0080gr.h>
#include <ctrl/ctrl0080/ctrl0080fifo.h>
#include <ctrl/ctrl0080/ctrl0080gpu.h>
#include <ctrl/ctrl90f1.h>
#include <ctrl/ctrla06f/ctrla06fgpfifo.h>
#include <ctrl/ctrl2080/ctrl2080fifo.h>
#include <ctrl/ctrl2080/ctrl2080bios.h>
#include <ctrl/ctrl2080/ctrl2080fb.h>
#include <ctrl/ctrl2080/ctrl2080gpu.h>
#include <ctrl/ctrl2080/ctrl2080gr.h>
#include <ctrl/ctrl2080/ctrl2080event.h>
#include <ctrl/ctrl2080/ctrl2080internal.h>
#include <ctrl/ctrl2080/ctrl2080ce.h>
#include "gpu/gsp/gsp_static_config.h"
#include "gsp/gsp_fw_wpr_meta.h"
#include "gsp/gsp_fw_sr_meta.h"
#include "gpu/gsp/gsp_fw_heap.h"
#include "gpu/gsp/gsp_init_args.h"
#include "gpu/gsp/kernel_gsp.h"
#include "rmgspseq.h"
#include "libos_init_args.h"
#include "rmRiscvUcode.h"
#include "msgq/msgq_priv.h"
#include "gpu/fifo/kernel_channel.h"
#include "gpu/mem_mgr/fbsr.h"


#include <nvctassert.h>
#define ABI_CHECK_SIZE_EQ(str, size)                 ct_assert(sizeof(str) == size)
#define ABI_CHECK_SIZE_GE(str, size)                 ct_assert(sizeof(str) >= size)
#define ABI_CHECK_ENUM_VAL_EQ(enumname, name, value) ct_assert(name == value)
#define ABI_CHECK_ENUM_VAL_GE(enumname, name, value) ct_assert(name >= value)
#define ABI_CHECK_OFFSET(str, fld, offset)           ct_assert(NV_OFFSETOF(str, fld) == offset)
#define ABI_CHECK_FIELD(str, fld, offset, size)      \
    ABI_CHECK_OFFSET(str, fld, offset);              \
    ABI_CHECK_SIZE_EQ((((str*)0)->fld), size)
#define ABI_CHECK_FIELD_FLEXIBLE(str, fld, offset)   \
    ABI_CHECK_OFFSET(str, fld, offset);              \
    ct_assert(offset <= sizeof(str))
)preabmle";
    fprintf(f, "%s\n", preamble);
}

int plugin_init(struct plugin_name_args* plugin_info, struct plugin_gcc_version* version) {
    if (!plugin_default_version_check(version, &gcc_version)) {
        ERROR("This GCC plugin is for a different version of GCC\n");
        return 1;
    }
    static struct plugin_info info = {
        "0.3",
        "GCC plugin to ABI-stability checks for NVIDIA GSP firmware, as used by nouveau",
    };
    register_callback(plugin_info->base_name, PLUGIN_INFO, NULL, &info);

    DBG("Starting plugin on %s\n", main_input_basename);
    if (!strcmp(gsp_abi_file, main_input_basename)) {
        register_callback(plugin_info->base_name, PLUGIN_FINISH_TYPE, finish_type_callback, NULL);
        register_callback(plugin_info->base_name, PLUGIN_FINISH_UNIT, finish_unit_callback, NULL);

        outfile = fopen("/tmp/gsp_abi_check.c", "w");
        if (!outfile) {
            ERROR("Failed to open output file\n");
            return 1;
        }
        write_preamble(outfile);
    }


    return 0;
}
