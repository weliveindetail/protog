#include <fcntl.h>
#include <stdlib.h>
#include <stdio.h>

#include <algorithm>
#include <functional>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include <google/protobuf/descriptor.h>
#include <google/protobuf/compiler/parser.h>
#include <google/protobuf/io/zero_copy_stream.h>
#include <google/protobuf/io/zero_copy_stream_impl.h>

using google::protobuf::Descriptor;
using google::protobuf::DescriptorPool;
using google::protobuf::FieldDescriptor;
using google::protobuf::FileDescriptor;
using google::protobuf::FileDescriptorProto;
using google::protobuf::compiler::Parser;
using google::protobuf::io::FileInputStream;
using google::protobuf::io::Tokenizer;
using google::protobuf::io::ZeroCopyInputStream;

#define LOG_TRACE 0

namespace protog {

enum class NodeType : int {
    BOOL = 1,
    LONG = 2,
    DOUBLE = 3,
    STRING = 4,
    OUTSIDE_OBJECT = 5,
    INSIDE_OBJECT = 6,
    ARRAY = 7,
};

static NodeType getNodeTypeForProtoType(FieldDescriptor::Type type) {
    switch (type) {
        case FieldDescriptor::TYPE_BOOL:
            return NodeType::BOOL;
        case FieldDescriptor::TYPE_INT32:
        case FieldDescriptor::TYPE_INT64:
        case FieldDescriptor::TYPE_UINT32:
        case FieldDescriptor::TYPE_FIXED32:
        case FieldDescriptor::TYPE_FIXED64:
        case FieldDescriptor::TYPE_SFIXED32:
        case FieldDescriptor::TYPE_SFIXED64:
        case FieldDescriptor::TYPE_SINT32:
        case FieldDescriptor::TYPE_SINT64:
            return NodeType::LONG;
        case FieldDescriptor::TYPE_FLOAT:
        case FieldDescriptor::TYPE_DOUBLE:
            return NodeType::DOUBLE;
        case FieldDescriptor::TYPE_STRING:
            return NodeType::STRING;
        case FieldDescriptor::TYPE_MESSAGE:
            return NodeType::OUTSIDE_OBJECT;
        case FieldDescriptor::TYPE_ENUM:
            return NodeType::LONG;
        case FieldDescriptor::TYPE_UINT64: // TODO
        case FieldDescriptor::TYPE_BYTES: // TODO
        default:
            throw std::runtime_error("Unsupported protobuf type " + std::to_string(static_cast<int>(type)));
    }
}

static std::string getTypeNameForFieldDesc(const FieldDescriptor &fieldDesc) {
    return fieldDesc.type() == FieldDescriptor::TYPE_MESSAGE ? fieldDesc.message_type()->name()
                                                             : fieldDesc.type_name();
}

static std::string replace_all(std::string str, const std::string& from, const std::string& to) {
    size_t start_pos = 0;
    while((start_pos = str.find(from, start_pos)) != std::string::npos) {
        str.replace(start_pos, from.length(), to);
        start_pos += to.length();
    }
    return str;
}

static std::vector<std::string> split(const std::string& str, const char delim, bool include_empty = false) {
    std::vector<std::string> result;
    std::stringstream ss{str};
    std::string item;
    while(std::getline(ss, item, delim)) {
        if (!item.empty() || include_empty) {
            result.push_back(item);
        }
    }
    return result;
}

struct Node {
    // structure
    Node *parent;
    std::vector<Node*> children;

    // node info
    NodeType type;
    int state;
    std::string name;
    std::string full_name; // including path
    std::string type_name;
    const Descriptor *desc;
    const FieldDescriptor *field;

    ~Node() {
        for(auto& child : children) {
            delete child;
        }
    }
};

struct Graph {
    std::string fname;
    std::string msgName;
    DescriptorPool pool;
    const FileDescriptor *fileDesc;
    const Descriptor *msgDesc;
    int stateCounter = 0;
    Node root;
    std::vector<Node *> all_nodes;
    std::vector<Node *> null_nodes;
    std::vector<Node *> bool_nodes;
    std::vector<Node *> long_nodes;
    std::vector<Node *> double_nodes;
    std::vector<Node *> string_nodes;
    std::vector<Node *> object_nodes;
    std::vector<Node *> key_nodes;
    std::vector<Node *> array_nodes;

    Graph(const std::string &fname, const std::string &msgName) : fname(fname), msgName(msgName) {
        root.state = ++stateCounter;

        int fd = open(fname.c_str(), O_RDONLY);
        if (fd < 0) {
            throw std::runtime_error("Unable to open proto file " + fname);
        }

        ZeroCopyInputStream *input = new FileInputStream(fd);
        Tokenizer tokenizer{input, nullptr};

        Parser parser;
        FileDescriptorProto protoDesc;
        if (!parser.Parse(&tokenizer, &protoDesc)) {
            throw std::runtime_error("Unable to parse proto file " + fname);
        }

        protoDesc.set_name("XXX"); // TODO: what is this name for?
        protoDesc.CheckInitialized();

#if LOG_TRACE == 1
        printf("Loaded file with following messages:\n");
        for (int i = 0; i < protoDesc.message_type_size(); ++i) {
            printf(">> %s.%s\n", protoDesc.package().c_str(), protoDesc.message_type(i).name().c_str());
        }
        printf("\n");
#endif

        fileDesc = pool.BuildFile(protoDesc);
        if (!fileDesc) {
            throw std::runtime_error("Unable to load proto file " + fname);
        }

        msgDesc = pool.FindMessageTypeByName(msgName);
        if (!msgDesc) {
            throw std::runtime_error("Unable to find message type " + msgName);
        }
    }

    void parseMessageDesc() {
        const auto &desc = *msgDesc;

        root.parent = nullptr;
        root.name = ".";
        root.full_name = ".";
        root.type_name = desc.name();
        root.type = NodeType::INSIDE_OBJECT;
        root.desc = &desc;
        root.field = nullptr;
        addNodeToTypeLists(root);

        parseMessageDescRec(desc, root);
    }

    void parseMessageDescRec(const Descriptor &desc, Node &node) {
        for (int f = 0; f < desc.field_count(); ++f) {
            const FieldDescriptor &fieldDesc = *desc.field(f);
            const auto type = getNodeTypeForProtoType(fieldDesc.type());
            const auto isRepeated = fieldDesc.is_repeated();

            Node &child = addChild(node);
            child.name = fieldDesc.name();
            child.full_name = node.full_name + child.name;
            child.field = &fieldDesc;
            child.desc = &desc;

            if (!isRepeated) {
                child.type = type;
                child.type_name = getTypeNameForFieldDesc(fieldDesc);
                addNodeToTypeLists(child);
                if (type == NodeType::OUTSIDE_OBJECT) {
                    Node& objChild = injectObjectNode(desc, fieldDesc, child);
                    parseMessageDescRec(*fieldDesc.message_type(), objChild);
                }
            } else {
                child.type = NodeType::ARRAY;
                child.type_name = "[" + getTypeNameForFieldDesc(fieldDesc) + "]";
                addNodeToTypeLists(child);
                Node& arrChild = injectArrayNode(desc, fieldDesc, type, child);
                if (type == NodeType::OUTSIDE_OBJECT) {
                    Node& objChild = injectObjectNode(desc, fieldDesc, arrChild);
                    parseMessageDescRec(*fieldDesc.message_type(), objChild);
                }
            }
        }
    }

    Node& injectArrayNode(const Descriptor &desc, const FieldDescriptor& fieldDesc, NodeType type, Node& node) {
        Node &arrChild = addChild(node);
        arrChild.name = fieldDesc.name();
        arrChild.full_name = node.full_name + "[]";
        arrChild.type = type; // specifies what type to expect in array
        arrChild.type_name = getTypeNameForFieldDesc(fieldDesc);
        arrChild.field = &fieldDesc;
        arrChild.desc = &desc;
        addNodeToTypeLists(arrChild);
        return arrChild;
    }

    Node& injectObjectNode(const Descriptor &desc, const FieldDescriptor& fieldDesc, Node& keyNode) {
        Node &objNode = addChild(keyNode);
        objNode.name = keyNode.name;
        objNode.full_name = keyNode.full_name + ".";
        objNode.type = NodeType::INSIDE_OBJECT;
        objNode.type_name = keyNode.type_name;
        objNode.field = &fieldDesc;
        objNode.desc = &desc;
        addNodeToTypeLists(objNode);
        return objNode;
    }

    void addNodeToTypeLists(Node &node) {
        assert(node.state);
        all_nodes.push_back(&node);
        if (node.field && node.field->is_optional()) {
            null_nodes.push_back(&node);
        }
        switch (node.type) {
            case NodeType::BOOL:
                bool_nodes.push_back(&node);
                // TODO: hack to allow 1/0 as true/false
                long_nodes.push_back(&node);
                break;
            case NodeType::LONG:
                long_nodes.push_back(&node);
                break;
            case NodeType::DOUBLE:
                double_nodes.push_back(&node);
                // TODO: hack to allow ints as doubles. shall we switch to number anyways?
                long_nodes.push_back(&node);
                break;
            case NodeType::STRING:
                string_nodes.push_back(&node);
                break;
            case NodeType::INSIDE_OBJECT:
                object_nodes.push_back(&node);
                break;
            case NodeType::OUTSIDE_OBJECT:
                key_nodes.push_back(&node);
                break;
            case NodeType::ARRAY:
                array_nodes.push_back(&node);
                break;
        }
    }

    Node &addChild(Node &node) {
        node.children.push_back(new Node());
        auto& child = *node.children.back();
        child.parent = &node;
        child.state = ++stateCounter;
        return child;
    }

    void printDebug(FILE* file) const {
        printDebugRec(file, root, 0);
    }

    void printDebugRec(FILE* file, const Node& node, int depth) const {
        printf(">> %s (type=%s, type_id=%d, state=%d\n", node.full_name.c_str(), node.type_name.c_str(), node.type, node.state);
        for (const auto &child : node.children) {
            printDebugRec(file, *child, depth + 1);
        }
    }
};

struct Printer {
    void print(const Graph &graph, const char* proto_header) {
        auto name_lower = graph.root.desc->name();
        std::transform(name_lower.begin(), name_lower.end(), name_lower.begin(), ::tolower);
        const auto cpp_type = get_full_cpp_type_name(*graph.root.desc);
        const auto res_name_prefix = name_lower + "_parser.pb";

        const auto header_name = res_name_prefix + ".h";
        FILE *header = fopen(header_name.c_str(), "w");
        printHeader(header, graph, name_lower.c_str(), cpp_type.c_str(), proto_header);
        fclose(header);

        const auto source_name = res_name_prefix + ".cpp";
        FILE *source = fopen(source_name.c_str(), "w");
        printSource(source, graph, name_lower.c_str(), cpp_type.c_str());
        fclose(source);
    }

    void printHeader(FILE *file, const Graph &graph, const char *t, const char *c, const char* h) {
        fprintf(file, "#pragma once\n\n");
        fprintf(file, "#include \"%s\"\n\n", h);
        printNamespaceBegin(file, graph);
        fprintf(file, "typedef struct %s_parser_state_s *%s_parser_state_t;\n", t, t);
        fprintf(file, "\n");
        fprintf(file, "%s %s_parser_easy(const std::string& json);\n", c, t);
        fprintf(file, "\n");
        fprintf(file, "%s_parser_state_t %s_parser_init(%s &msg);\n", t, t, c);
        fprintf(file, "void %s_parser_free(%s_parser_state_t state);\n", t, t);
        fprintf(file, "int %s_parser_on_chunk(%s_parser_state_t state, char *chunk, size_t chunkLen);\n", t, t);
        fprintf(file, "int %s_parser_complete(%s_parser_state_t state);\n", t, t);
        fprintf(file, "int %s_parser_reset(%s_parser_state_t state);\n", t, t);
        fprintf(file, "char *%s_parser_get_error(%s_parser_state_t state);\n", t, t);
        fprintf(file,
                "char *%s_parser_get_error(%s_parser_state_t state, int verbose, const char *chunk, size_t chunkLen);\n",
                t, t);
        fprintf(file, "void %s_parser_free_error(%s_parser_state_t state, char *err);\n", t, t);
        fprintf(file, "\n");
        printNamespaceEnd(file, graph);
    }

    void printSource(FILE *file, const Graph &graph, const char *t, const char *c) {
        printSourceIncludes(file, t);
        printNamespaceBegin(file, graph);
        printTypeDefinition(file, t, c);
        fprintf(file, "namespace {\n\n");
        printSourceImpl(file, graph, t, c);
        printYajlCallbacks(file, t);
        fprintf(file, "} // anonymous namespace\n\n");
        printApiImpl(file, t, c);
        printNamespaceEnd(file, graph);
    }

    void printSourceIncludes(FILE *file, const char *t) {
        fprintf(file, "#include \"%s_parser.pb.h\"\n\n", t);
        fprintf(file, "#include <stdlib.h>\n");
        fprintf(file, "#include <stdio.h>\n\n");
        fprintf(file, "#include <functional>\n");
        fprintf(file, "#include <string>\n\n");
        fprintf(file, "#include <yajl/yajl_parse.h>\n");
        fprintf(file, "\n");
    }

    void printTypeDefinition(FILE *file, const char *t, const char *c) {
        fprintf(file, "struct %s_parser_config_s {\n", t);
        fprintf(file, "    bool checkInitialized;\n");
        fprintf(file, "};\n");
        fprintf(file, "\n");
        fprintf(file, "struct %s_parser_state_s {\n", t);
        fprintf(file, "    %s_parser_state_s(%s &req) : req(req) { }\n\n", t, c);
        fprintf(file, "    %s_parser_config_s config;\n", t);
        fprintf(file, "    yajl_handle handle = NULL;\n");
        fprintf(file, "    size_t location = 0;\n");
        fprintf(file, "    %s &req;\n", c);
        fprintf(file, "    std::vector<::google::protobuf::Message *> msgStack;\n\n");
        fprintf(file, "    void reset() {\n");
        fprintf(file, "        location = 0;\n");
        fprintf(file, "        req.Clear();\n");
        fprintf(file, "        msgStack.clear();\n");
        fprintf(file, "    }\n");
        fprintf(file, "};\n");
        fprintf(file, "\n");
    }

    void printSourceImpl(FILE *file, const Graph &graph, const char *t, const char *c) {
        printNullImpl(file, t, c, graph.null_nodes);
        printPodImpl(file, t, c, "boolean", "int", graph.bool_nodes);
        printPodImpl(file, t, c, "integer", "long long", graph.long_nodes);
        printPodImpl(file, t, c, "double", "double", graph.double_nodes);
        printStringImpl(file, t, c, graph.string_nodes);
        printMapStartImpl(file, graph.object_nodes, t, c);
        printMapKeyImpl(file, t, c, graph.object_nodes);
        printMapEndImpl(file, t, c, graph.object_nodes);
        printArrayStartImpl(file, t, c, graph.array_nodes);
        printArrayEndImpl(file, t, c, graph.array_nodes);
    }

    void printNullImpl(FILE* file, const char* t, const char* c, const std::vector<Node*>& nodes) {
        fprintf(file, "static int %s_parser_impl_parse_null(void *ctx) {\n", t);
        fprintf(file, "    %s_parser_state_s &state = *static_cast<%s_parser_state_t>(ctx);\n", t, t);
        fprintf(file, "    switch (state.location) {\n");
        for (const auto& node : nodes) {
            assert(node);
            printNullStateImpl(file, *node);
        }
        fprintf(file, "        default:\n");
        fprintf(file, "            fprintf(stderr, \"State %%zu does not allow null\\n\", state.location);\n");
        fprintf(file, "            exit(1);\n");
        fprintf(file, "    }\n");
        fprintf(file, "    return 1;\n");
        fprintf(file, "}\n\n");
    }

    void printNullStateImpl(FILE* file, const Node& node) {
        const auto cpp_type = get_full_cpp_type_name(*node.desc);
        fprintf(file, "        case %d: // key %s\n", node.state, node.full_name.c_str());
        fprintf(file, "            static_cast<%s *>(state.msgStack.back())->clear_%s();\n", cpp_type.c_str(), node.name.c_str());
        fprintf(file, "            state.location = %d;\n", node.parent->state);
        fprintf(file, "            break;\n");
    }

    void printPodImpl(FILE* file, const char* t, const char* c, const char* p, const char* pt, const std::vector<Node*>& nodes) {
        fprintf(file, "static int %s_parser_impl_parse_%s(void *ctx, %s v) {\n", t, p, pt);
        fprintf(file, "    %s_parser_state_s &state = *static_cast<%s_parser_state_t>(ctx);\n", t, t);
        fprintf(file, "    switch (state.location) {\n");
        for (const auto& node : nodes) {
            assert(node);
            printPodStateImpl(file, *node);
        }
        fprintf(file, "        default:\n");
        fprintf(file, "            fprintf(stderr, \"State %%zu does not allow %s\\n\", state.location);\n", p);
        fprintf(file, "            exit(1);\n");
        fprintf(file, "    }\n");
        fprintf(file, "    return 1;\n");
        fprintf(file, "}\n\n");
    }

    void printPodStateImpl(FILE* file, const Node& node) {
        const auto cpp_type = get_full_cpp_type_name(*node.desc);
        fprintf(file, "        case %d: // key %s\n", node.state, node.full_name.c_str());
        fprintf(file, "            static_cast<%s *>(state.msgStack.back())->", cpp_type.c_str());
        if (node.field->is_repeated()) {
            fprintf(file, "add");
        } else {
            fprintf(file, "set");
        }
        fprintf(file, "_%s(", node.name.c_str());
        if (node.field->type() == FieldDescriptor::TYPE_ENUM) {
            const auto enum_type = get_full_cpp_type_name(*node.field->enum_type());
            fprintf(file, "\n                    static_cast<%s>(v)", enum_type.c_str());
        } else {
            fprintf(file, "v");
        }
        fprintf(file, ");\n");
        if (!node.field->is_repeated()) { // in case of array, the closing bracket will clean up
            fprintf(file, "            state.location = %d;\n", node.parent->state);
        }
        fprintf(file, "            break;\n");
    }

    void printStringImpl(FILE* file, const char* t, const char* c, const std::vector<Node*>& nodes) {
        fprintf(file, "static int %s_parser_impl_parse_string(void *ctx, const unsigned char *v, size_t vLen) {\n", t);
        fprintf(file, "    %s_parser_state_s &state = *static_cast<%s_parser_state_t>(ctx);\n", t, t);
        fprintf(file, "    std::string *target = nullptr;\n");
        fprintf(file, "    switch (state.location) {\n");
        for (const auto& node : nodes) {
            assert(node);
            printStringStateImpl(file, *node);
        }
        fprintf(file, "        default:\n");
        fprintf(file, "            fprintf(stderr, \"State %%zu does not allow string\\n\", state.location);\n");
        fprintf(file, "            exit(1);\n");
        fprintf(file, "    }\n");
        fprintf(file, "    if (target) {\n");
        fprintf(file, "        target->resize(vLen, '\\0');\n");
        fprintf(file, "        memcpy(const_cast<char*>(target->c_str()), v, vLen);\n");
        fprintf(file, "        const_cast<char*>(target->c_str())[vLen] = '\\0';\n");
        fprintf(file, "    }\n");
        fprintf(file, "    return 1;\n");
        fprintf(file, "}\n\n");
    }

    void printStringStateImpl(FILE* file, const Node& node) {
        const auto cpp_type = get_full_cpp_type_name(*node.desc);
        const char* verb = node.field->is_repeated() ? "add" : "mutable";
        fprintf(file, "        case %d: // key %s\n", node.state, node.full_name.c_str());
        fprintf(file, "            target = static_cast<%s *>(state.msgStack.back())->%s_%s();\n", cpp_type.c_str(), verb, node.name.c_str());
        if (!node.field->is_repeated()) { // in case of array, the closing bracket will clean up
            fprintf(file, "            state.location = %d;\n", node.parent->state);
        }
        fprintf(file, "            break;\n");
    }

    void printMapStartImpl(FILE *file, const std::vector<Node *> &nodes, const char *t, const char *c) {
        fprintf(file, "static int %s_parser_impl_parse_start_map(void *ctx) {\n", t);
        fprintf(file, "    %s_parser_state_s &state = *static_cast<%s_parser_state_t>(ctx);\n", t, t);
        fprintf(file, "    switch (state.location) {\n");
        for (const auto& node : nodes) {
            assert(node);
            printMapStartStateImpl(file, *node, t);
        }
        fprintf(file, "        default:\n");
        fprintf(file, "            fprintf(stderr, \"State %%zu does not allow object\\n\", state.location);\n");
        fprintf(file, "            exit(1);\n");
        fprintf(file, "    }\n");
        fprintf(file, "    return 1;\n");
        fprintf(file, "}\n\n");
    }

    void printMapStartStateImpl(FILE* file, const Node& node, const char* t) {
        if (!node.parent) {
            fprintf(file, "        case 0: // map .\n");
            fprintf(file, "            state.location = %d;\n", node.state);
            fprintf(file, "            assert(state.msgStack.empty());\n");
            fprintf(file, "            state.msgStack.push_back(&state.req);\n");
            fprintf(file, "            break;\n");
        } else {
            const auto cpp_type = get_full_cpp_type_name(*node.desc);
            const char* verb = node.field->is_repeated() ? "add" : "mutable";
            fprintf(file, "        case %d: // map %s\n", node.parent->state, node.full_name.c_str());
            fprintf(file, "            state.location = %d;\n", node.state);
            fprintf(file, "            state.msgStack.push_back(static_cast<%s *>(state.msgStack.back())->%s_%s());\n", cpp_type.c_str(), verb, node.name.c_str());
            fprintf(file, "            break;\n");
        }
    }

    void printMapKeyImpl(FILE* file, const char* t, const char* c, const std::vector<Node*>& nodes) {
        fprintf(file, "static int %s_parser_impl_parse_map_key(void *ctx, const unsigned char *key_, size_t keyLen) {\n", t);
        fprintf(file, "    const auto key = std::string{reinterpret_cast<const char *>(key_), keyLen};\n");
        fprintf(file, "    %s_parser_state_s &state = *static_cast<%s_parser_state_t>(ctx);\n", t, t);
        fprintf(file, "    const auto hash = std::hash<std::string>()(key);\n");
        fprintf(file, "    switch (state.location) {\n");
        for (const auto& node : nodes) {
            assert(node);
            printMapKeyStateImpl(file, *node);
        }
        fprintf(file, "        default:\n");
        fprintf(file, "            fprintf(stderr, \"Location %%zu does not allow the key %%s\\n\", state.location, key.c_str());\n");
        fprintf(file, "            exit(1);\n");
        fprintf(file, "    }\n");
        fprintf(file, "    return 1;\n");
        fprintf(file, "}\n\n");
    }

    void printMapKeyStateImpl(FILE* file, const Node& node) {
        fprintf(file, "        case %d: // map %s\n", node.state, node.full_name.c_str());
        fprintf(file, "            switch (hash) {\n");
        for (const auto& child : node.children) {
            const auto hash = std::hash<std::string>()(child->name);
            fprintf(file, "                case %zuu: // %s\n", hash, child->name.c_str());
            fprintf(file, "                    state.location = %d;\n", child->state);
            fprintf(file, "                    break;\n");
        }
        fprintf(file, "                default:\n");
        fprintf(file, "                    fprintf(stderr, \"Invalid key %s for %%s\\n\", key.c_str());\n", node.full_name.c_str());
        fprintf(file, "                    exit(1);\n");
        fprintf(file, "            }\n");
        fprintf(file, "            break;\n");
    }

    void printMapEndImpl(FILE* file, const char* t, const char* c, const std::vector<Node*>& nodes) {
        fprintf(file, "static int %s_parser_impl_parse_end_map(void *ctx) {\n", t);
        fprintf(file, "    %s_parser_state_s &state = *static_cast<%s_parser_state_t>(ctx);\n", t, t);
        fprintf(file, "    if (state.config.checkInitialized) {\n");
        fprintf(file, "        state.msgStack.back()->CheckInitialized();\n");
        fprintf(file, "    }\n");
        fprintf(file, "    switch (state.location) {\n");
        for (const auto& node : nodes) {
            assert(node);
            printMapEndStateImpl(file, *node);
        }
        fprintf(file, "        default:\n");
        fprintf(file, "            fprintf(stderr, \"State %%zu does not allow closing object\\n\", state.location);\n");
        fprintf(file, "            exit(1);\n");
        fprintf(file, "    }\n");
        fprintf(file, "    return 1;\n");
        fprintf(file, "}\n\n");
    }

    void printMapEndStateImpl(FILE* file, const Node& node) {
        if (!node.parent || !node.parent->parent) {
            fprintf(file, "        case %d: // map .\n", node.state);
            fprintf(file, "            state.location = 0;\n");
            fprintf(file, "            state.msgStack.pop_back();\n");
            fprintf(file, "            assert(state.msgStack.empty());\n");
            fprintf(file, "            break;\n");
        } else {
            const auto cpp_type = get_full_cpp_type_name(*node.desc);
            fprintf(file, "        case %d: // map %s\n", node.state, node.full_name.c_str());
            assert(node.parent && node.parent->parent);
            if (node.parent && node.parent->parent && node.parent->parent->type == NodeType::ARRAY) {
                fprintf(file, "            state.location = %d;\n", node.parent->state);
            } else {
                fprintf(file, "            state.location = %d;\n", node.parent->parent->state);
            }
            fprintf(file, "            state.msgStack.pop_back();\n");
            fprintf(file, "            break;\n");
        }
    }

    void printArrayStartImpl(FILE* file, const char* t, const char* c, const std::vector<Node*>& nodes) {
        fprintf(file, "static int %s_parser_impl_parse_start_array(void *ctx) {\n", t);
        fprintf(file, "    %s_parser_state_s &state = *static_cast<%s_parser_state_t>(ctx);\n", t, t);
        fprintf(file, "    switch (state.location) {\n");
        for (const auto& node : nodes) {
            assert(node);
            printArrayStartStateImpl(file, *node);
        }
        fprintf(file, "        default:\n");
        fprintf(file, "            fprintf(stderr, \"State %%zu does not allow array\\n\", state.location);\n");
        fprintf(file, "            exit(1);\n");
        fprintf(file, "    }\n");
        fprintf(file, "    return 1;\n");
        fprintf(file, "}\n\n");
    }

    void printArrayStartStateImpl(FILE* file, const Node& node) {
        assert(node.children.size() == 1);
        fprintf(file, "        case %d: // key %s\n", node.state, node.full_name.c_str());
        fprintf(file, "            state.location = %d;\n", node.children[0]->state);
        fprintf(file, "            break;\n");
    }

    void printArrayEndImpl(FILE* file, const char* t, const char* c, const std::vector<Node*>& nodes) {
        fprintf(file, "static int %s_parser_impl_parse_end_array(void *ctx) {\n", t);
        fprintf(file, "    %s_parser_state_s &state = *static_cast<%s_parser_state_t>(ctx);\n", t, t);
        fprintf(file, "    switch (state.location) {\n");
        for (const auto& node : nodes) {
            assert(node);
            printArrayEndStateImpl(file, *node);
        }
        fprintf(file, "        default:\n");
        fprintf(file, "            fprintf(stderr, \"State %%zu does not allow closing array\\n\", state.location);\n");
        fprintf(file, "            exit(1);\n");
        fprintf(file, "    }\n");
        fprintf(file, "    return 1;\n");
        fprintf(file, "}\n\n");
    }

    void printArrayEndStateImpl(FILE* file, const Node& node) {
        // TODO: fix arrays in root object case?!
        assert(node.parent);
        assert(node.children.size() == 1);
        fprintf(file, "        case %d: // key %s\n", node.children[0]->state, node.full_name.c_str());
        fprintf(file, "            state.location = %d;\n", node.parent->state);
        fprintf(file, "            break;\n");
    }

    void printYajlCallbacks(FILE *file, const char *t) {
        fprintf(file, "static yajl_callbacks %s_parser_impl_callbacks = {\n", t);
        fprintf(file, "        %s_parser_impl_parse_null,\n", t);
        fprintf(file, "        %s_parser_impl_parse_boolean,\n", t);
        fprintf(file, "        %s_parser_impl_parse_integer,\n", t);
        fprintf(file, "        %s_parser_impl_parse_double,\n", t);
        fprintf(file, "        NULL, //number,\n");
        fprintf(file, "        %s_parser_impl_parse_string,\n", t);
        fprintf(file, "        %s_parser_impl_parse_start_map,\n", t);
        fprintf(file, "        %s_parser_impl_parse_map_key,\n", t);
        fprintf(file, "        %s_parser_impl_parse_end_map,\n", t);
        fprintf(file, "        %s_parser_impl_parse_start_array,\n", t);
        fprintf(file, "        %s_parser_impl_parse_end_array,\n", t);
        fprintf(file, "};\n\n");
    }

    void printApiImpl(FILE *file, const char *t, const char *c) {
        fprintf(file, "%s %s_parser_easy(const std::string &json) {\n", c, t);
        fprintf(file, "    %s msg;\n", c);
        fprintf(file, "    %s_parser_state_t state = %s_parser_init(msg);\n", t, t);
        fprintf(file, "\n");
        fprintf(file, "    int rc = %s_parser_on_chunk(state, const_cast<char*>(json.c_str()), json.size());\n", t);
        fprintf(file, "    if (rc != 0) {\n");
        fprintf(file, "        char *err = %s_parser_get_error(state);\n", t);
        fprintf(file, "        throw std::runtime_error(err);\n");
        fprintf(file, "    }\n");
        fprintf(file, "\n");
        fprintf(file, "    rc = %s_parser_complete(state);\n", t);
        fprintf(file, "    if (rc != 0) {\n");
        fprintf(file, "        char *err = %s_parser_get_error(state);\n", t);
        fprintf(file, "        throw std::runtime_error(err);\n");
        fprintf(file, "    }\n");
        fprintf(file, "\n");
        fprintf(file, "    %s_parser_free(state);\n", t);
        fprintf(file, "\n");
        fprintf(file, "    return msg;\n");
        fprintf(file, "}\n");
        fprintf(file, "\n");
        fprintf(file, "%s_parser_state_t %s_parser_init(%s &msg) {\n", t, t, c);
        fprintf(file, "    %s_parser_state_t state = new %s_parser_state_s(msg);\n", t, t);
        fprintf(file, "    state->config.checkInitialized = true;\n");
        fprintf(file, "\n");
        fprintf(file, "    yajl_handle handle = yajl_alloc(&%s_parser_impl_callbacks, NULL, state);\n", t);
        fprintf(file, "    yajl_config(handle, yajl_allow_comments, 0);\n");
        fprintf(file, "    yajl_config(handle, yajl_dont_validate_strings, 0);\n");
        fprintf(file, "    yajl_config(handle, yajl_allow_trailing_garbage, 0);\n");
        fprintf(file, "    yajl_config(handle, yajl_allow_multiple_values, 0);\n");
        fprintf(file, "    yajl_config(handle, yajl_allow_partial_values, 0);\n");
        fprintf(file, "    state->handle = handle;\n");
        fprintf(file, "\n");
        fprintf(file, "    return state;\n");
        fprintf(file, "}\n");
        fprintf(file, "\n");
        fprintf(file, "void %s_parser_free(%s_parser_state_t state) {\n", t, t);
        fprintf(file, "    assert(state);\n");
        fprintf(file, "    if (state) {\n");
        fprintf(file, "        if (state->handle) {\n");
        fprintf(file, "            yajl_free(state->handle);\n");
        fprintf(file, "        }\n");
        fprintf(file, "        delete state;\n");
        fprintf(file, "    }\n");
        fprintf(file, "}\n");
        fprintf(file, "\n");
        fprintf(file, "int %s_parser_on_chunk(%s_parser_state_t state, char *chunk, size_t chunkLen) {\n", t, t);
        fprintf(file, "    assert(state);\n");
        fprintf(file, "    assert(state->handle);\n");
        fprintf(file, "    const unsigned char *uChunk = reinterpret_cast<const unsigned char *>(chunk);\n");
        fprintf(file, "    int stat = yajl_parse(state->handle, uChunk, chunkLen);\n");
        fprintf(file, "    return stat != yajl_status_ok;\n");
        fprintf(file, "}\n");
        fprintf(file, "\n");
        fprintf(file, "int %s_parser_complete(%s_parser_state_t state) {\n", t, t);
        fprintf(file, "    assert(state);\n");
        fprintf(file, "    assert(state->handle);\n");
        fprintf(file, "    int stat = yajl_complete_parse(state->handle);\n");
        fprintf(file, "    return stat != yajl_status_ok;\n");
        fprintf(file, "}\n");
        fprintf(file, "\n");
        fprintf(file, "int %s_parser_reset(%s_parser_state_t state) {\n", t, t);
        fprintf(file, "    assert(state);\n");
        fprintf(file, "    assert(state->handle);\n");
        fprintf(file, "    if (state && state->handle) {\n");
        fprintf(file, "        state->reset();\n");
        fprintf(file, "    }\n");
        fprintf(file, "    return 0;\n");
        fprintf(file, "}\n");
        fprintf(file, "\n");
        fprintf(file, "char *%s_parser_get_error(%s_parser_state_t state) {\n", t, t);
        fprintf(file, "    return %s_parser_get_error(state, 0, 0, 0);\n", t);
        fprintf(file, "}\n");
        fprintf(file, "\n");
        fprintf(file, "char *%s_parser_get_error(%s_parser_state_t state, int verbose, const char *chunk,\n", t, t);
        fprintf(file, "                                  size_t chunkLen) {\n");
        fprintf(file, "    assert(state);\n");
        fprintf(file, "    assert(state->handle);\n");
        fprintf(file, "    const unsigned char *uChunk = reinterpret_cast<const unsigned char *>(chunk);\n");
        fprintf(file, "    unsigned char *err = nullptr;\n");
        fprintf(file, "    if (state && state->handle) {\n");
        fprintf(file, "        err = yajl_get_error(state->handle, verbose, uChunk, chunkLen);\n");
        fprintf(file, "    }\n");
        fprintf(file, "    return reinterpret_cast<char *>(err);\n");
        fprintf(file, "}\n");
        fprintf(file, "\n");
        fprintf(file, "void %s_parser_free_error(%s_parser_state_t state, char *err) {\n", t, t);
        fprintf(file, "    assert(state);\n");
        fprintf(file, "    assert(state->handle);\n");
        fprintf(file, "    if (state && state->handle) {\n");
        fprintf(file, "        yajl_free_error(state->handle, reinterpret_cast<unsigned char *>(err));\n");
        fprintf(file, "    }\n");
        fprintf(file, "}\n\n");
    }

    void printNamespaceBegin(FILE *file, const Graph &graph) {
        const auto ns = split(graph.fileDesc->package(), '.');
        for (auto it = ns.begin(); it != ns.end(); ++it) {
            fprintf(file, "namespace %s {\n", it->c_str());
        }
        fprintf(file, "\n");
    }

    void printNamespaceEnd(FILE *file, const Graph &graph) {
        const auto ns = split(graph.fileDesc->package(), '.');
        for (auto it = ns.rbegin(); it != ns.rend(); ++it) {
            fprintf(file, "} // namespace %s\n", it->c_str());
        }
    }

    template <typename Descriptor>
    static std::string get_full_cpp_type_name(const Descriptor& desc) {
        return "::" + replace_all(desc.full_name(), ".", "::");
    }
};
}

int main(int argc, char **argv) {
    // TODO: parse args
    if (argc != 4) {
        fprintf(stderr, "You must provide the following arguments:\n");
        fprintf(stderr, "$ %s <message proto> <generated header> <full message class name>\n", argv[0]);
        fprintf(stderr, "e.g.:\n");
        fprintf(stderr, "$ %s mymessage.proto mymessage.pb.h some.ns.MyMessage\n", argv[0]);
        exit(1);
    }
    const char* fname = argv[1];
    const char* proto_header = argv[2];
    const char* message_name = argv[3];

    protog::Graph graph{fname, message_name};
    graph.parseMessageDesc();
#if LOG_TRACE == 1
    graph.printDebug(stdout);
#endif

    protog::Printer printer{};
    printer.print(graph, proto_header);

    return 0;
}
