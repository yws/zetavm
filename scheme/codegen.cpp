#include <cassert>
#include <vector>
#include <string>
#include <iostream>
#include "codegen.h"

// Last assigned id number
size_t lastIdNo = 0;

class Block
{
private:

    size_t idNo;

    std::vector<std::string> instrs;

    bool finalized = false;

public:

    Block()
    {
        idNo = lastIdNo++;
    }

    std::string getHandle() const { return "block_" + std::to_string(idNo); }

    bool isFinalized() const { return finalized; }

    void add(std::string instrStr)
    {
        if (finalized)
        {
            std::cout << "block is finalized:" << std::endl;
            for (auto str: instrs)
                std::cout << "    " + str << std::endl;
            std::cout << "cannot add:" << std::endl;
            std::cout << instrStr << std::endl;
            exit(-1);
        }

        instrs.push_back("{ " + instrStr + " }");
    };

    void finalize(std::string& out)
    {
        assert (!finalized);
        assert (instrs.size() > 0);

        out += getHandle() + " = {\n";
        out += "  instrs: [\n";

        for (auto str: instrs)
            out += "    " + str + ",\n";

        out += "  ]\n";
        out += "};\n\n";

        finalized = true;
    }
};

struct Function
{
private:

    size_t idNo;

    // Visible function parameter names
    std::vector<std::string> params;

    // Functions always have at least 1 local
    // to store the hidden function/closure argument
    size_t numLocals = 1;

    Block* entryBlock;

    /// Local variable indices
    std::unordered_map<std::string, size_t> localIdxs;

public:

    Function(
        std::vector<std::string> params,
        Block* entryBlock
    )
    : params(params),
      entryBlock(entryBlock)
    {
        idNo = lastIdNo++;
    }

    std::string getHandle() { return "fun_" + std::to_string(idNo); }

    /// Register a local variable declaration
    void registerDecl(std::string identName)
    {
        assert (entryBlock != nullptr);

        if (localIdxs.find(identName) == localIdxs.end())
        {
            size_t newIdx = localIdxs.size();
            localIdxs[identName] = newIdx;
            numLocals++;
        }
    }

    bool hasLocal(std::string identName)
    {
        return (localIdxs.find(identName) != localIdxs.end());
    }

    size_t getLocalIdx(std::string identName)
    {
        assert (hasLocal(identName));
        return localIdxs[identName];
    }

    void finalize(std::string& out)
    {
        assert (entryBlock != nullptr);

        std::string paramsStr = "[";
        for (auto param: params)
            paramsStr += "\'" + param + "\',";
        paramsStr += "]";

        out += getHandle() + " = {\n";
        out += "  entry:@" + entryBlock->getHandle() + ",\n";
        out += "  params:" + paramsStr + ",\n";
        out += "  num_locals:" + std::to_string(numLocals) + ",\n";
        out += "};\n\n";

        entryBlock = nullptr;
    }
};

class CodeGenCtx
{
public:

    std::string& out;

    /// Current function being generated
    Function* fun;

    /// Current block into which to insert
    Block* curBlock;

    /// Loop continue block
    Block* contBlock;

    /// Loop break block
    Block* breakBlock;

    /// Unit function flag
    bool unitFun;

    CodeGenCtx(
        std::string& out,
        Function* fun,
        bool unitFun,
        Block* curBlock,
        Block* contBlock = nullptr,
        Block* breakBlock = nullptr
    )
    : out(out),
      fun(fun),
      curBlock(curBlock),
      contBlock(contBlock),
      breakBlock(breakBlock)
    {
    }

    /// Create a sub-context of this context
    CodeGenCtx subCtx(
        Block* startBlock = nullptr,
        Block* contBlock = nullptr,
        Block* breakBlock = nullptr
    )
    {
        return CodeGenCtx(
            this->out,
            this->fun,
            this->unitFun,
            startBlock? startBlock:this->curBlock,
            contBlock? contBlock:this->contBlock,
            breakBlock? breakBlock:this->breakBlock
        );
    }

    /// Continue code generation at a given block
    void merge(Block* block)
    {
        curBlock = block;
    }

    /// Add an instruction string
    void addStr(std::string instrStr)
    {
        assert (curBlock != nullptr);
        curBlock->add(instrStr);
    }

    /// Add an instruction with no arguments by opcode name
    void addOp(std::string opStr)
    {
        addStr("op:'" + opStr + "'");
    }

    /// Add a push instruction
    void addPush(std::string valStr)
    {
        addStr("op:'push', val:" + valStr);
    }

    /// Add a branch instruction
    void addBranch(
        std::string op,
        std::string name0 = "",
        Block* target0 = nullptr,
        std::string name1 = "",
        Block* target1 = nullptr,
        std::string extraArgs = ""
    )
    {
        assert (curBlock != nullptr);

        if (target0 && target1)
        {
            addStr(
                "op:'" + op + "', " +
                name0 + ":@" + target0->getHandle() + ", " +
                name1 + ":@" + target1->getHandle() +
                (extraArgs.length()? ", ":"") + extraArgs
            );
        }
        else if (target0)
        {
            addStr(
                "op:'" + op + "', " + name0 + ":@" + target0->getHandle() +
                (extraArgs.length()? ", ":"") + extraArgs
            );
        }
        else
        {
            addStr(
                "op:'" + op + "'" +
                (extraArgs.length()? ", ":"") + extraArgs
            );
        }

        /// Serialize the basic block
        curBlock->finalize(this->out);
    }
};

// Forward declarations
static void genValue(CodeGenCtx& ctx, Value* value);

/**
Generate code for a code unit
*/
std::string genProgram(std::unique_ptr<Program> program)
{
    std::string out = "#zeta-image\n\n";

    Block* entryBlock = new Block();

    Function* unitFun = new Function(std::vector<std::string>(), entryBlock);

    // Create the initial context
    CodeGenCtx ctx(
        out,
        unitFun,
        true,
        entryBlock
    );

    // Define the global and exports objects
    out += "exports_obj = { init: @" + unitFun->getHandle() + " };\n";
    out += "global_obj = { exports: @exports_obj };\n\n";

    // Import the base runtime.
    ctx.addStr("op:'push', val:@global_obj");
    ctx.addStr("op:'push', val:'rt'");
    ctx.addStr("op:'push', val:'scheme/runtime.zim'");
    ctx.addStr("op:'import'");
    ctx.addStr("op:'set_field'");

    // Generate code for each value
    for (auto &value : program->values)
    {
        genValue(ctx, value.get());
        ctx.addStr("op:'pop'");
    }

    // Add a final return statement to the unit function
    if (!ctx.curBlock->isFinalized())
    {
        ctx.addStr("op:'push', val:$true");
        ctx.addBranch("ret");
    }

    // Generate output for the unit function
    unitFun->finalize(out);

    // Export the exports object
    out += "@exports_obj;\n";

    return out;
}

/**
Generate code for calling a function
*/
static void
genCall(CodeGenCtx& ctx, std::string funName, size_t numArgs, bool isRuntimeCall)
{
    // Setup the global object to find the call field
    ctx.addStr("op:'push', val:@global_obj");

    // If this is a runtime call, the get the 'rt' field first.
    if (isRuntimeCall)
    {
        ctx.addStr("op:'push', val:'rt'");
        ctx.addStr("op:'get_field'");
    }

    // Get the method being called.
    ctx.addStr("op:'push', val:'" + funName + "'");
    ctx.addOp("get_field");

    // Call it.
    auto contBlock = new Block();
    ctx.addBranch(
        "call",
        "ret_to", contBlock,
        "", nullptr,
        "num_args:" + std::to_string(numArgs)
    );
    ctx.merge(contBlock);
}

/**
Check if the given pair is a tagged list
*/
static bool isTaggedList(Pair* pair, const std::string &name)
{
    if (auto identifier = dynamic_cast<Identifier*>(pair->car.get()))
    {
        return identifier->val == name;
    }

    return false;
}

/**
Generate code for the given value
*/
static void genValue(CodeGenCtx& ctx, Value* value)
{
    // Nothing to do here
    if (!value)
    {
        return;
    }

    // Push an integer
    if (auto integer = dynamic_cast<Integer*>(value))
    {
        ctx.addStr("op:'push', val:" + integer->toString());
    }

    // Push a string
    else if (auto str = dynamic_cast<String*>(value))
    {
        ctx.addStr("op:'push', val:" + str->toString());
    }

    // Recursively generate code on the pair
    else if (auto pair = dynamic_cast<Pair*>(value))
    {
        if (isTaggedList(pair, "write"))
        {
            genValue(ctx, pair->cdr.get());
            genCall(ctx, "write", 1, true);
        }

        else if (isTaggedList(pair, "newline"))
        {
            genValue(ctx, pair->cdr.get());
            genCall(ctx, "newline", 0, true);
        }

        else
        {
            genValue(ctx, pair->car.get());
            genValue(ctx, pair->cdr.get());
        }
    }
}
