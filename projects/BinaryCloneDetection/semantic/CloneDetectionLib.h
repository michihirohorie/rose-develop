#ifndef RSIM_CloneDetection_H
#define RSIM_CloneDetection_H

#include "sqlite3x.h"
#include "PartialSymbolicSemantics.h"
#include "x86InstructionSemantics.h"
#include "BinaryPointerDetection.h"
#include "SqlDatabase.h"
#include "LinearCongruentialGenerator.h"

#include <stdint.h>
#include <vector>
#include <ostream>
#include <map>

namespace CloneDetection {

extern const char *schema; /**< Contents of Schema.sql file, initialized in CloneDetectionSchema.C */

using namespace BinaryAnalysis::InstructionSemantics;

typedef std::set<SgAsmFunction*> Functions;
typedef std::map<SgAsmFunction*, int> FunctionIdMap;
typedef std::map<int, SgAsmFunction*> IdFunctionMap;
typedef std::map<rose_addr_t, int> AddressIdMap;
typedef BinaryAnalysis::FunctionCall::Graph CG;
typedef boost::graph_traits<CG>::vertex_descriptor CG_Vertex;
enum Verbosity { SILENT, LACONIC, EFFUSIVE };

/*******************************************************************************************************************************
 *                                      Progress bars
 *******************************************************************************************************************************/

/** Show progress bar indicator on standard error. The progress bar's constructor should indicate the total number of times
 *  that the show() method is expected to be called.  If the standard error stream is a terminal (or force_output is set) then
 *  a progress bar is emitted at most once per RTP_INTERVAL seconds (default 1).  The cursor is positioned at the beginning
 *  of the progress bar's line each time the progress bar is printed. The destructor erases the progress bar. */
class Progress {
protected:
    size_t cur, total;
    time_t last_report;
    bool is_terminal, force, had_output;
    std::string mesg;
    enum { WIDTH=100, RPT_INTERVAL=1 };
    void init();
public:
    Progress(size_t total): cur(0), total(total), last_report(0), is_terminal(false), force(false), had_output(false) { init(); }
    ~Progress() { clear(); }

    /** Force the progress bar to be emitted even if standard error is not a terminal. */
    void force_output(bool b) { force = b; }

    /** Increment the progress. The bar is updated only if it's been at least RPT_INTERVAL seconds since the previous update
     *  or if @p update_now is true.
     *  @{ */
    void increment(bool update_now=false);
    Progress& operator++() { increment(); return *this; }
    Progress operator++(int) { Progress retval=*this; increment(); return retval; }
    /** @} */

    /** Immediately erase the progress bar from the screen by emitting white space. */
    void clear();

    /** Reset the progress counter back to zero. */
    void reset(size_t current=0, size_t total=(size_t)(-1));

    /** Show a message. The progress bar is updated only if it's been at least RPT_INTERVAL seconds since the previous
     *  update or if @p update_now is true.*/
    void message(const std::string&, bool update_now=true);

    /** Update the progress bar without incrementing. The progress bar is updated only if it's been at least RPT_INTERVAL
     *  seconds since the previous update or if @p update_now is true. */
    void update(bool update_now=true);

    /** Returns the current rendering of the progress line. */
    std::string line() const;

    /** Returns the current position. */
    size_t current() const { return cur; }
};


/*******************************************************************************************************************************
 *                                      Analysis faults
 *******************************************************************************************************************************/

/** Special output values for when something goes wrong. */
class AnalysisFault {
public:
    enum Fault {
        NONE        = 0,
        DISASSEMBLY = 911000001,     /**< Disassembly failed possibly due to bad address. */
        INSN_LIMIT  = 911000002,     /**< Maximum number of instructions executed. */
        HALT        = 911000003,     /**< x86 HLT instruction executed. */
        INTERRUPT   = 911000004,     /**< x86 INT instruction executed. */
        SEMANTICS   = 911000005,     /**< Some fatal problem with instruction semantics, such as a not-handled instruction. */
        SMTSOLVER   = 911000006,     /**< Some fault in the SMT solver. */
        INPUT_LIMIT = 911000007,     /**< Too many input values consumed. */
    };
    
    /** Return the short name of a fault ID. */
    static const char *fault_name(Fault fault) {
        switch (fault) {
            case NONE:          return "";
            case DISASSEMBLY:   return "FAULT_DISASSEMBLY";
            case INSN_LIMIT:    return "FAULT_INSN_LIMIT";
            case HALT:          return "FAULT_HALT";
            case INTERRUPT:     return "FAULT_INTERRUPT";
            case SEMANTICS:     return "FAULT_SEMANTICS";
            case SMTSOLVER:     return "FAULT_SMTSOLVER";
            case INPUT_LIMIT:   return "FAULT_INPUT_LIMIT";
            default:
                assert(!"fault not handled");
                abort();
        }
    }
};


/*******************************************************************************************************************************
 *                                      Exceptions
 *******************************************************************************************************************************/

/** Exception thrown by this semantics domain. */
class Exception {
public:
    Exception(const std::string &mesg): mesg(mesg) {}
    std::string mesg;
};

/** Exceptions thrown by the analysis semantics to indicate some kind of fault. */
class FaultException: public Exception {
public:
    AnalysisFault::Fault fault;
    FaultException(AnalysisFault::Fault fault)
        : Exception(std::string("encountered ") + AnalysisFault::fault_name(fault)), fault(fault) {}
};

/*******************************************************************************************************************************
 *                                      File names table
 *******************************************************************************************************************************/

class FilesTable {
public:
    struct Row {
        Row(): in_db(false), id(-1) {}
        Row(int id, const std::string &name, const std::string &digest, const std::string &ast_digest, bool in_db)
            : in_db(in_db), id(id), name(name), digest(digest), ast_digest(ast_digest) {}
        bool in_db;
        int id;
        std::string name;
        std::string digest;             // SHA1 of this file if it is's stored in the semantic_binaries table
        std::string ast_digest;         // SHA1 hash of binary AST if it's stored in the semantic_binaries table
    };
    typedef std::map<int, Row> Rows;
    Rows rows;
    typedef std::map<std::string, int> NameIdx;
    NameIdx name_idx;
    int next_id;

    /** Constructor loads file information from the database. */
    FilesTable(const SqlDatabase::TransactionPtr &tx): next_id(0) { load(tx); }

    /** Reload information from the database. */
    void load(const SqlDatabase::TransactionPtr &tx);

    /** Save all unsaved files to the database. */
    void save(const SqlDatabase::TransactionPtr &tx);

    /** Add or remove an AST for this file. Returns the SHA1 digest for the AST, which also serves as the key in the
     *  semantic_binaries table. */
    std::string save_ast(const SqlDatabase::TransactionPtr&, int64_t cmd_id, int file_id, SgProject*);

    /** Load an AST from the database if it is saved there. Returns the SgProject or null. */
    SgProject *load_ast(const SqlDatabase::TransactionPtr&, int file_id);

    /** Add (or update) file content to the database. Returns the SHA1 digest for the file. */
    std::string add_content(const SqlDatabase::TransactionPtr&, int64_t cmd_id, int file_id);

    void clear() {
        rows.clear();
        name_idx.clear();
        next_id = 0;
    }

    int insert(const std::string &name);
    int id(const std::string &name) const;
    std::string name(int id) const;

};


/*******************************************************************************************************************************
 *                                      Output groups
 *******************************************************************************************************************************/

typedef std::map<int, uint64_t> IdVa;
typedef std::map<uint64_t, int> VaId;

// How to store values: 1 => vector; 2 => set
#define OUTPUTGROUP_VALUE_CONTAINER 2

// Should we store call graph info?
#undef OUTPUTGROUP_SAVE_CALLGRAPH

// Should we store system call info?
#undef OUTPUTGROUP_SAVE_SYSCALLS

/** Collection of output values. The output values are gathered from the instruction semantics state after a specimen function
 *  is analyzed.  The outputs consist of those interesting registers that are marked as having been written to by the specimen
 *  function, and the memory values whose memory cells are marked as having been written to.  We omit status flags since they
 *  are not typically treated as function call results, and we omit the instruction pointer (EIP). */
class OutputGroup {
public:
    typedef uint32_t value_type;
#if OUTPUTGROUP_VALUE_CONTAINER == 1
    typedef std::vector<value_type> Values;
#else
    typedef std::set<value_type> Values;
#endif
    OutputGroup(): fault(AnalysisFault::NONE), ninsns(0) {}
    bool operator<(const OutputGroup &other) const;
    bool operator==(const OutputGroup &other) const;
    void clear();
    void print(std::ostream&, const std::string &title="", const std::string &prefix="") const;
    void print(RTS_Message*, const std::string &title="", const std::string &prefix="") const;
    friend std::ostream& operator<<(std::ostream &o, const OutputGroup &outputs) {
        outputs.print(o);
        return o;
    }
    void add_param(const std::string vtype, int pos, int64_t value); // used by OutputGroups
    void add_value(int64_t value) {
#if OUTPUTGROUP_VALUE_CONTAINER == 1
        values.push_back(value);
#else
        values.insert(value);
#endif
    }

    Values values;
    std::vector<int> callee_ids;                // IDs for called functions
    std::vector<int> syscalls;                  // system call numbers in the order they occur
    AnalysisFault::Fault fault;
    size_t ninsns;                              // number of instructions executed
};

// Used internally by OutputGroups so that we don't have to store OutputGroup objects in std::map. OutputGroup objects can be
// very large, causing std::map reallocation to be super slow.  Therefore, the map stores OutputGroupDesc objects which have
// an operator< that compares the pointed-to OutputGroup objects.
class OutputGroupDesc {
public:
    explicit OutputGroupDesc(const OutputGroup *ogroup): ptr(ogroup) { assert(ogroup!=NULL); }
    bool operator<(const OutputGroupDesc &other) const {
        assert(ptr!=NULL && other.ptr!=NULL);
        return *ptr < *other.ptr;
    }
private:
    const OutputGroup *ptr;
};

/** A collection of OutputGroup objects, each having an ID number.  ID numbers are 63-bit random numbers (non-negative 63-bit
 *  values) so that we don't have to synchronize between processes that are creating them. */
class OutputGroups {
public:
    /** Constructor creates an empty OutputGroups. */
    OutputGroups(): file(NULL) {}

    /** Constructor initializes this OutputGroup with the contents of the database. */
    explicit OutputGroups(const SqlDatabase::TransactionPtr &tx): file(NULL) { load(tx); }

    ~OutputGroups();

    /** Reload this output group with the contents from the database. */
    void load(const SqlDatabase::TransactionPtr&);

    /** Reload only the specified hashkey and return a pointer to it. Returns false if the object does not exist in the
     * database (and also removes it from memory if it existed there). */
    bool load(const SqlDatabase::TransactionPtr&, int64_t hashkey);

    /** Insert a new OutputGroup locally.  This does not update the database. If @p hashkey is not the default -1 then
     *  this function assumes that the hash key was obtained from the database and therefore we are adding an OutputGroup
     *  object that is already in the database; such an object will not be written back to the database by a save()
     *  operation. */
    int64_t insert(const OutputGroup&, int64_t hashkey=-1);

    /** Erase the specified output group according to its hash key. */
    void erase(int64_t hashkey);

    /** Find the hashkey for an existing OutputGroup. Returns -1 if the specified OutputGroup does not exist. */
    int64_t find(const OutputGroup&) const;

    /** Does the given hashkey exist? */
    bool exists(int64_t hashkey) const;

    /** Find the output group for the given hash key. Returns NULL if the hashkey does not exist. */
    const OutputGroup* lookup(int64_t hashkey) const;

    /** Save locally-inserted OutputGroup objects to the database. */
    void save(const SqlDatabase::TransactionPtr&);

    /** Generate another hash key. */
    int64_t generate_hashkey();

    /** Return the set of all output group hash keys. */
    std::vector<int64_t> hashkeys() const;

private:
    typedef std::map<int64_t/*hashkey*/, OutputGroup*> IdOutputMap;
    typedef std::map<OutputGroupDesc, int64_t/*hashkey*/> OutputIdMap;
    IdOutputMap ogroups;
    OutputIdMap ids;
    LinearCongruentialGenerator lcg; // to generate ID numbers
    std::string filename;
    FILE *file; // temp file for new rows
};

/*******************************************************************************************************************************
 *                                      Instruction Providor
 *******************************************************************************************************************************/

/** Efficient mapping from address to instruction. */
class InstructionProvidor {
protected:
    typedef std::map<rose_addr_t, SgAsmInstruction*> Addr2Insn;
    Addr2Insn addr2insn;
public:
    InstructionProvidor() {}
    InstructionProvidor(const Functions &functions) {
        for (Functions::const_iterator fi=functions.begin(); fi!=functions.end(); ++fi)
            insert(*fi);
    }
    InstructionProvidor(const std::vector<SgAsmFunction*> &functions) {
        for (size_t i=0; i<functions.size(); ++i)
            insert(functions[i]);
    }
    InstructionProvidor(SgNode *ast) {
        std::vector<SgAsmFunction*> functions = SageInterface::querySubTree<SgAsmFunction>(ast);
        for (size_t i=0; i<functions.size(); ++i)
            insert(functions[i]);
    }
    void insert(SgAsmFunction *func) {
        std::vector<SgAsmInstruction*> insns = SageInterface::querySubTree<SgAsmInstruction>(func);
        for (size_t i=0; i<insns.size(); ++i)
            addr2insn[insns[i]->get_address()] = insns[i];
    }
    SgAsmInstruction *get_instruction(rose_addr_t addr) const {
        Addr2Insn::const_iterator found = addr2insn.find(addr);
        return found==addr2insn.end() ? NULL : found->second;
    }
};

typedef BinaryAnalysis::PointerAnalysis::PointerDetection<InstructionProvidor> PointerDetector;

/*******************************************************************************************************************************
 *                                      Address hasher
 *******************************************************************************************************************************/

/** Hashes a virtual address to a small integer. */
class AddressHasher {
public:
    AddressHasher() { init_table(); }
    AddressHasher(unsigned seed): lcg(seed) { init_table(); }

    void init_table() {
        for (size_t i=0; i<256; ++i)
            tab[i] = lcg() % 256;
    }
    
    uint8_t operator()(rose_addr_t addr, Verbosity verbosity) {
        uint8_t retval = 0;
        for (size_t i=0; i<4; ++i) {
            uint8_t byte = IntegerOps::shiftRightLogical2(addr, 8*i) & 0xff;
            retval = tab[(retval+byte) & 0xff];
        }
        if (verbosity>=EFFUSIVE)
            std::cerr <<"CloneDetection: initializing memory[" <<StringUtility::addrToString(addr) <<"]"
                      <<" = (uint8_t)" <<(unsigned)retval <<"\n";
        return retval;
    }
private:
    LinearCongruentialGenerator lcg;
    uint8_t tab[256];
};
    
/*******************************************************************************************************************************
 *                                      Input Group
 *******************************************************************************************************************************/

/** Initial values to supply for inputs.  These are defined in terms of integers which are then cast to the appropriate size
 *  when needed.  During fuzz testing, whenever the specimen reads from a register or memory location which has never been
 *  written, we consume the next value from this input object. When all values are consumed, this object begins to return only
 *  zero values. */
class InputGroup {
public:
    enum Type { INTEGER, POINTER, UNKNOWN_TYPE };
    InputGroup(): next_integer_(0), next_pointer_(0), limit_consumption_(false) {}
    InputGroup(const SqlDatabase::TransactionPtr &tx, int id): next_integer_(0), next_pointer_(0), limit_consumption_(false) {
        load(tx, id);
    }
    bool load(const SqlDatabase::TransactionPtr&, int id);
    void add_integer(uint64_t i) { integers_.push_back(i); }
    void add_pointer(uint64_t p) { pointers_.push_back(p); }
    size_t size() const { return integers_.size() + pointers_.size(); }
    void limit_consumption(bool b) { limit_consumption_=b; }
    uint64_t next_integer() {
        if (limit_consumption_ && next_integer_ >= integers_.size())
            throw FaultException(AnalysisFault::INPUT_LIMIT);
        uint64_t retval = next_integer_ < integers_.size() ? integers_[next_integer_] : 0;
        ++next_integer_; // increment even past the end so we know how many inputs were consumed
        return retval;
    }
    uint64_t next_pointer() {
        if (limit_consumption_ && next_pointer_ >= pointers_.size())
            throw FaultException(AnalysisFault::INPUT_LIMIT);
        uint64_t retval = next_pointer_ < pointers_.size() ? pointers_[next_pointer_] : 0;
        ++next_pointer_; // increment even past the end so we know how many inputs were consumed
        return retval;
    }
    size_t integers_consumed() const { return next_integer_; }
    size_t pointers_consumed() const { return next_pointer_; }
    const std::vector<uint64_t> get_integers() const { return integers_; }
    const std::vector<uint64_t> get_pointers() const { return pointers_; }
    size_t nconsumed() const { return integers_consumed() + pointers_consumed(); }
    void reset() { next_integer_ = next_pointer_ = 0; }
    void clear() {
        reset();
        integers_.clear();
        pointers_.clear();
    }
    void shuffle() {
        for (size_t i=0; i<integers_.size(); ++i) {
            size_t j = rand() % integers_.size();
            std::swap(integers_[i], integers_[j]);
        }
        for (size_t i=0; i<pointers_.size(); ++i) {
            size_t j = rand() % pointers_.size();
            std::swap(pointers_[i], pointers_[j]);
        }
    }
    std::string toString() const {
        std::ostringstream ss;
        print(ss);
        return ss.str();
    }
    void print(std::ostream &o) const {
        o <<"integer inputs (" <<integers_.size() <<" total):\n";
        for (size_t i=0; i<integers_.size(); ++i)
            o <<"  " <<integers_[i] <<(i==next_integer_?"\t<-- next input":"") <<"\n";
        if (next_integer_>=integers_.size())
            o <<"  all integers have been consumed; returning zero\n";
        o <<"pointer inputs (" <<pointers_.size() <<" total):\n";
        for (size_t i=0; i<pointers_.size(); ++i)
            o <<"  " <<pointers_[i] <<(i==next_pointer_?"\t<-- next input":"") <<"\n";
        if (next_pointer_>=pointers_.size())
            o <<"  all pointers have been consumed; returning null\n";
    }
        
protected:
    std::vector<uint64_t> integers_;
    std::vector<uint64_t> pointers_;
    size_t next_integer_, next_pointer_;        // May increment past the end of its array
    bool limit_consumption_;                    // throw exeption if we consume too much?
};

/****************************************************************************************************************************
 *                                      Semantic policy parameters
 ****************************************************************************************************************************/

struct PolicyParams {
    PolicyParams()
        : timeout(5000), verbosity(SILENT), follow_calls(false), init_memory(false),
          initial_stack(0x80000000) {}
    size_t timeout;                     /**< Maximum number of instrutions per fuzz test before giving up. */
    Verbosity verbosity;                /**< Produce lots of output?  Traces each instruction as it is simulated. */
    bool follow_calls;                  /**< Follow CALL instructions if possible rather than consuming an input? */
    bool init_memory;                   /**< Initialize most of memory with an address hash function seeded with an input. */
    rose_addr_t initial_stack;          /**< Initial values for ESP and EBP. */
};


/*******************************************************************************************************************************
 *                                      Analysis Machine State
 *******************************************************************************************************************************/

/** Bits to track variable access. */
enum {
    NO_ACCESS=0,                        /**< Variable has been neither read nor written. */
    HAS_BEEN_READ=1,                    /**< Variable has been read. */
    HAS_BEEN_WRITTEN=2                  /**< Variable has been written. */ 
};

/** Semantic value to track read/write state of registers. The basic idea is that we have a separate register state object
 *  whose values are instances of this ReadWriteState type. We can use the same RegisterStateX86 template for the read/write
 *  state as we do for the real register state. */
template<size_t nBits>
struct ReadWriteState {
    unsigned state;                     /**< Bit vector containing HAS_BEEN_READ and/or HAS_BEEN_WRITTEN, or zero. */
    ReadWriteState(): state(NO_ACCESS) {}
};

/** One value stored in memory. */
struct MemoryValue {
    MemoryValue(): val(PartialSymbolicSemantics::ValueType<8>(0)), rw_state(NO_ACCESS) {}
    MemoryValue(const PartialSymbolicSemantics::ValueType<8> &val, unsigned rw_state): val(val), rw_state(rw_state) {}
    PartialSymbolicSemantics::ValueType<8> val;
    unsigned rw_state;
};

/** Analysis machine state. We override some of the memory operations. All values are concrete (we're using
 *  PartialSymbolicSemantics only for its constant-folding ability and because we don't yet have a specifically concrete
 *  semantics domain). */
template <template <size_t> class ValueType>
class State: public PartialSymbolicSemantics::State<ValueType> {
public:
    typedef std::map<uint32_t, MemoryValue> MemoryCells;        // memory cells indexed by address
    MemoryCells stack_cells;                                    // memory state for stack memory (accessed via SS register)
    MemoryCells data_cells;                                     // memory state for anything that non-stack (e.g., DS register)
    BaseSemantics::RegisterStateX86<ValueType> registers;
    BaseSemantics::RegisterStateX86<ReadWriteState> register_rw_state;
    OutputGroup output_group;                                  // output values filled in as we run a function

    // Write a single byte to memory. The rw_state are the HAS_BEEN_READ and/or HAS_BEEN_WRITTEN bits.
    void mem_write_byte(X86SegmentRegister sr, const ValueType<32> &addr, const ValueType<8> &value,
                        unsigned rw_state=HAS_BEEN_WRITTEN) {
        MemoryCells &cells = x86_segreg_ss==sr ? stack_cells : data_cells;
        cells[addr.known_value()] = MemoryValue(value, rw_state);
    }
        
    // Read a single byte from memory.  If the read operation cannot find an appropriate memory cell, then @p
    // uninitialized_read is set (it is not cleared in the counter case).
    ValueType<8> mem_read_byte(X86SegmentRegister sr, const ValueType<32> &addr, bool *uninitialized_read/*out*/) {
        MemoryCells &cells = x86_segreg_ss==sr ? stack_cells : data_cells;

        std::vector<ValueType<8> > found;
        typename MemoryCells::iterator ci = cells.find(addr.known_value());
        if (ci!=cells.end())
            found.push_back(ci->second.val);
        if (!found.empty())
            return found[rand()%found.size()];
        *uninitialized_read = true;
        return ValueType<8>(rand()%256);
    }
        
    // Returns true if two memory addresses can be equal.
    static bool may_alias(const ValueType<32> &addr1, const ValueType<32> &addr2) {
        return addr1.known_value()==addr2.known_value();
    }

    // Returns true if two memory address are equivalent.
    static bool must_alias(const ValueType<32> &addr1, const ValueType<32> &addr2) {
        return addr1.known_value()==addr2.known_value();
    }

    // Reset the analysis state by clearing all memory and by resetting the read/written status of all registers.
    void reset_for_analysis() {
        stack_cells.clear();
        data_cells.clear();
        registers.clear();
        register_rw_state.clear();
        output_group.clear();
    }

    // Return output values.  These are the interesting general-purpose registers to which a value has been written, and the
    // memory locations to which a value has been written.  The returned object can be deleted when no longer needed.  The EIP,
    // ESP, and EBP registers are not considered to be interesting.  Memory addresses that are less than or equal to the @p
    // stack_frame_top but larger than @p stack_frame_top - @p frame_size are not considered to be outputs (they are the
    // function's local variables). The @p stack_frame_top is usually the address of the function's return EIP, the address
    // that was pushed onto the stack by the CALL instruction.
    //
    // Even though we're operating in the concrete domain, it is possible for a register or memory location to contain a
    // non-concrete value.  This can happen if only a sub-part of the register was written (e.g., writing a concrete value to
    // AX will result in EAX still having a non-concrete value.
    OutputGroup get_outputs(uint32_t stack_frame_top, size_t frame_size, Verbosity verbosity) {
        OutputGroup outputs = this->output_group;

        // Function return value is EAX, but only if it has been written to and is concrete
        if (0 != (register_rw_state.gpr[x86_gpr_ax].state & HAS_BEEN_WRITTEN) && registers.gpr[x86_gpr_ax].is_known()) {
            if (verbosity>=EFFUSIVE)
                std::cerr <<"output for ax = " <<registers.gpr[x86_gpr_ax].known_value() <<"\n";
            outputs.add_value(registers.gpr[x86_gpr_ax].known_value());
        }

        // Add to the outputs the memory cells that are outside the local stack frame (estimated) and are concrete
        for (MemoryCells::iterator ci=stack_cells.begin(); ci!=stack_cells.end(); ++ci) {
            uint32_t addr = ci->first;
            MemoryValue &mval = ci->second;
            bool cell_in_frame = (addr <= stack_frame_top && addr > stack_frame_top-frame_size);
            if (0 != (mval.rw_state & HAS_BEEN_WRITTEN) && mval.val.is_known()) {
                if (verbosity>=EFFUSIVE)
                    std::cerr <<"output for stack address " <<StringUtility::addrToString(addr) <<": "
                              <<mval.val <<(cell_in_frame?" (IGNORED)":"") <<"\n";
                if (!cell_in_frame)
                    outputs.add_value(mval.val.known_value());
            }
        }

        // Add to the outputs the non-stack memory cells
        for (MemoryCells::iterator ci=data_cells.begin(); ci!=data_cells.end(); ++ci) {
            uint32_t addr = ci->first;
            MemoryValue &mval = ci->second;
            if (0 != (mval.rw_state & HAS_BEEN_WRITTEN) && mval.val.is_known()) {
                if (verbosity>=EFFUSIVE)
                    std::cerr <<"output for data address " <<addr <<": " <<mval.val <<"\n";
                outputs.add_value(mval.val.known_value());
            }
        }

        return outputs;
    }

    // Printing
    void print(std::ostream &o, unsigned domain_mask=0x07) const {
        BaseSemantics::SEMANTIC_NO_PRINT_HELPER *helper = NULL;
        this->registers.print(o, "   ", helper);
        for (size_t i=0; i<2; ++i) {
            size_t ncells=0, max_ncells=100;
            const MemoryCells &cells = 0==i ? stack_cells : data_cells;
            o <<"== Memory (" <<(0==i?"stack":"data") <<" segment) ==\n";
            for (typename MemoryCells::const_iterator ci=cells.begin(); ci!=cells.end(); ++ci) {
                uint32_t addr = ci->first;
                const MemoryValue &mval = ci->second;
                if (++ncells>max_ncells) {
                    o <<"    skipping " <<cells.size()-(ncells-1) <<" more memory cells for brevity's sake...\n";
                    break;
                }
                o <<"         cell access:"
                  <<(0==(mval.rw_state & HAS_BEEN_READ)?"":" read")
                  <<(0==(mval.rw_state & HAS_BEEN_WRITTEN)?"":" written")
                  <<(0==(mval.rw_state & (HAS_BEEN_READ|HAS_BEEN_WRITTEN))?" none":"")
                  <<"\n"
                  <<"    address symbolic: " <<addr <<"\n";
                o <<"        value " <<mval.val <<"\n";
            }
        }
    }
        
    friend std::ostream& operator<<(std::ostream &o, const State &state) {
        state.print(o);
        return o;
    }
};

/*******************************************************************************************************************************
 *                                      Analysis Semantic Policy
 *******************************************************************************************************************************/

// Define the template portion of the CloneDetection::Policy so we don't have to repeat it over and over in the method
// defintions found in CloneDetectionTpl.h.  This also helps Emac's c++-mode auto indentation engine since it seems to
// get confused by complex multi-line templates.
#define CLONE_DETECTION_TEMPLATE template <                                                                                    \
    template <template <size_t> class ValueType> class State,                                                                  \
    template <size_t nBits> class ValueType                                                                                    \
>

CLONE_DETECTION_TEMPLATE
class Policy: public PartialSymbolicSemantics::Policy<State, ValueType> {
public:
    typedef          PartialSymbolicSemantics::Policy<State, ValueType> Super;

    State<ValueType> state;
    static const rose_addr_t FUNC_RET_ADDR = 4083;      // Special return address to mark end of analysis
    InputGroup *inputs;                                 // Input values to use when reading a never-before-written variable
    const PointerDetector *pointers;                    // Addresses of pointer variables, or null if not analyzed
    SgAsmInterpretation *interp;                        // Interpretation in which we're executing
    size_t ninsns;                                      // Number of instructions processed since last trigger() call
    AddressHasher address_hasher;                       // Hashes a virtual address
    const InstructionProvidor *insns;                   // Instruction cache
    PolicyParams params;                                // Parameters for controlling the policy
    AddressIdMap entry2id;                              // Map from function entry address to function ID

    Policy(const PolicyParams &params, const AddressIdMap &entry2id)
        : inputs(NULL), pointers(NULL), ninsns(0), params(params), entry2id(entry2id) {}

    template <size_t nBits>
    ValueType<nBits>
    next_input_value(InputGroup::Type type) {
        // Instruction semantics API1 calls readRegister when initializing X86InstructionSemantics in order to obtain the
        // original EIP value, but we haven't yet set up an input group (nor would we want this initialization to consume
        // an input anyway).  So just return zero.
        if (!inputs)
            return ValueType<nBits>(0);

        uint64_t value = 0;
        size_t nvalues = 0;
        const char *type_name = NULL;
        switch (type) {
            case InputGroup::POINTER:
                value = inputs->next_pointer();
                nvalues = inputs->pointers_consumed();
                type_name = "pointer";
                break;
            case InputGroup::UNKNOWN_TYPE:
            case InputGroup::INTEGER:
                value = inputs->next_integer();
                nvalues = inputs->integers_consumed();
                type_name = "integer";
                break;
        }

        ValueType<nBits> retval(value);
        if (params.verbosity>=EFFUSIVE)
            std::cerr <<"CloneDetection: using " <<type_name <<" input #" <<nvalues <<": " <<retval <<"\n";
        return retval;
    }

    // Return output values. These include the return value, certain memory writes, function calls, system calls, etc.
    OutputGroup get_outputs() {
        return state.get_outputs(params.initial_stack, 8192, params.verbosity);
    }
    
    // Sets up the machine state to start the analysis of one function.
    void reset(SgAsmInterpretation *interp, SgAsmFunction *func, InputGroup *inputs, const InstructionProvidor *insns,
               const PointerDetector *pointers/*=NULL*/) {
        inputs->reset();
        this->inputs = inputs;
        this->insns = insns;
        this->pointers = pointers;
        this->ninsns = 0;
        this->interp = interp;
        state.reset_for_analysis();
        if (params.init_memory)
            address_hasher = AddressHasher(inputs->next_integer());

        // Initialize some registers.  Obviously, the EIP register needs to be set, but we also set the ESP and EBP to known
        // (but arbitrary) values so we can detect when the function returns.  Be sure to use these same values in related
        // analyses (like pointer variable detection).
        rose_addr_t target_va = func->get_entry_va();
        ValueType<32> eip(target_va);
        this->writeRegister("eip", eip);
        ValueType<32> esp(params.initial_stack); // stack grows down
        this->writeRegister("esp", esp);
        ValueType<32> ebp(params.initial_stack);
        this->writeRegister("ebp", ebp);

        // Initialize callee-saved registers. The callee-saved registers interfere with the analysis because if the same
        // function is compiled two different ways, then it might use different numbers of callee-saved registers.  Since
        // callee-saved registers are pushed onto the stack without first initializing them, the push consumes an input.
        // Therefore, we must consistently initialize all possible callee-saved registers.  We are assuming cdecl calling
        // convention (i.e., GCC's default for C/C++).
        ValueType<32> rval(inputs->next_integer());
        this->writeRegister("ebx", rval);
        this->writeRegister("esi", rval);
        this->writeRegister("edi", rval);

        // Initialize some additional registers.  GCC optimization sometimes preserves a register's value as part of a code
        // path that's shared between points when the register has been initialized and when it hasn't.  Non-optimized code
        // (apparently) never does this.  Therefore, we need to also initialize registers used this way.
        this->writeRegister("eax", rval);
        this->writeRegister("ecx", rval);
        this->writeRegister("edx", rval);
    }
    
    void startInstruction(SgAsmInstruction *insn_) /*override*/ {
        if (++ninsns >= params.timeout)
            throw FaultException(AnalysisFault::INSN_LIMIT);
        SgAsmx86Instruction *insn = isSgAsmx86Instruction(insn_);
        assert(insn!=NULL);
        if (params.verbosity>=EFFUSIVE) {
            std::cerr <<"CloneDetection: " <<std::string(80, '-') <<"\n"
                      <<"CloneDetection: executing: " <<unparseInstructionWithAddress(insn) <<"\n";
        }

        // Make sure EIP is updated with the instruction's address (most policies assert this).
        this->writeRegister("eip", ValueType<32>(insn->get_address()));

        Super::startInstruction(insn_);
    }
        

    // Special handling for some instructions. Like CALL, which does not call the function but rather consumes an input value.
    void finishInstruction(SgAsmInstruction *insn) /*override*/ {
        SgAsmx86Instruction *insn_x86 = isSgAsmx86Instruction(insn);
        assert(insn_x86!=NULL);
        ++state.output_group.ninsns;

        // Special handling for function calls.  Optionally, instead of calling the function, we treat the function as
        // returning a newly consumed input value to the caller via EAX.  We make the following assumptions:
        //    * Function calls are via CALL instruction
        //    * The called function always returns
        //    * The called function's return value is in the EAX register
        //    * The caller cleans up any arguments that were passed via stack
        //    * The function's return value is an integer (non-pointer) type
        if (x86_call==insn_x86->get_kind()) {
            bool follow = params.follow_calls;
            ValueType<32> callee_va = this->template readRegister<32>("eip");
            if (follow) {
                SgAsmInstruction *called_insn = insns->get_instruction(callee_va.known_value());
                SgAsmFunction *called_func = called_insn ? SageInterface::getEnclosingNode<SgAsmFunction>(called_insn) : NULL;
                if ((follow = called_insn!=NULL && called_func!=NULL)) {
                    std::string func_name = called_func->get_name();
                    if (func_name.size()>4 && 0==func_name.substr(func_name.size()-4).compare("@plt"))
                        follow = false;
                }
            }
            if (!follow) {
                if (params.verbosity>=EFFUSIVE)
                    std::cerr <<"CloneDetection: special handling for function call (fall through and return via EAX)\n";
#ifdef OUTPUTGROUP_SAVE_CALLGRAPH
                AddressIdMap::const_iterator found = entry2id.find(callee_va.known_value());
                if (found!=entry2id.end())
                    state.output_group.callee_ids.push_back(found->second);
#endif
                ValueType<32> call_fallthrough_va = this->template number<32>(insn->get_address() + insn->get_size());
                this->writeRegister("eip", call_fallthrough_va);
                this->writeRegister("eax", next_input_value<32>(InputGroup::INTEGER));
                ValueType<32> esp = this->template readRegister<32>("esp");
                esp = this->add(esp, ValueType<32>(4));
                this->writeRegister("esp", esp);
            }
        }

        Super::finishInstruction(insn);
    }
    
    // Handle INT 0x80 instructions: save the system call number (from EAX) in the output group and set EAX to a random
    // value, thus consuming one input.
    void interrupt(uint8_t inum) /*override*/ {
        if (0x80==inum) {
            if (params.verbosity>=EFFUSIVE)
                std::cerr <<"CloneDetection: special handling for system call (fall through and consume an input into EAX)\n";
#ifdef OUTPUTGROUP_SAVE_SYSCALLS
            ValueType<32> syscall_num = this->template readRegister<32>("eax");
            state.output_group.syscalls.push_back(syscall_num.known_value());
#endif
            this->writeRegister("eax", next_input_value<32>(InputGroup::INTEGER));
        } else {
            Super::interrupt(inum);
            throw FaultException(AnalysisFault::INTERRUPT);
        }
    }

    // Handle the HLT instruction by throwing an exception.
    void hlt() {
        throw FaultException(AnalysisFault::HALT);
    }

    // Track memory access.
    template<size_t nBits>
    ValueType<nBits> readMemory(X86SegmentRegister sr, ValueType<32> a0, const ValueType<1> &cond) {
        // For RET instructions, when reading DWORD PTR ss:[INITIAL_STACK], do not consume an input, but rather
        // return FUNC_RET_ADDR.
        SgAsmx86Instruction *insn = isSgAsmx86Instruction(this->get_insn());
        if (32==nBits && insn && x86_ret==insn->get_kind()) {
            rose_addr_t c_addr = a0.known_value();
            if (c_addr == this->params.initial_stack)
                return ValueType<nBits>(this->FUNC_RET_ADDR);
        }

        // Read a multi-byte value from memory in little-endian order.
        bool uninitialized_read = false; // set to true by any mem_read_byte() that has no data
        assert(8==nBits || 16==nBits || 32==nBits);
        ValueType<32> dword = this->concat(state.mem_read_byte(sr, a0, &uninitialized_read),
                                           ValueType<24>(0));
        if (nBits>=16) {
            ValueType<32> a1 = this->add(a0, ValueType<32>(1));
            dword = this->or_(dword, this->concat(ValueType<8>(0),
                                                  this->concat(state.mem_read_byte(sr, a1, &uninitialized_read),
                                                               ValueType<16>(0))));
        }
        if (nBits>=24) {
            ValueType<32> a2 = this->add(a0, ValueType<32>(2));
            dword = this->or_(dword, this->concat(ValueType<16>(0),
                                                  this->concat(state.mem_read_byte(sr, a2, &uninitialized_read),
                                                               ValueType<8>(0))));
        }
        if (nBits>=32) {
            ValueType<32> a3 = this->add(a0, ValueType<32>(3));
            dword = this->or_(dword, this->concat(ValueType<24>(0), state.mem_read_byte(sr, a3, &uninitialized_read)));
        }

        ValueType<nBits> retval = this->template extract<0, nBits>(dword);
        if (uninitialized_read) {
            // At least one of the bytes read did not previously exist, so we need to initialize these memory locations.
            // Sometimes we want memory to have a value that depends on the next input, and other times we want a value that
            // depends on the address.
            bool consume_input = !params.init_memory;
            if (consume_input && this->interp!=NULL && this->interp->get_map()!=NULL) {
                consume_input = this->interp->get_map()->exists(a0.known_value());
            }
            if (consume_input) {
                // Return either a pointer or integer value depending pointer detection analysis
                SymbolicSemantics::ValueType<32> a0_sym(a0.known_value());
                InputGroup::Type type = this->pointers!=NULL && this->pointers->is_pointer(a0_sym) ?
                                        InputGroup::POINTER : InputGroup::INTEGER;
                retval = next_input_value<nBits>(type);
            } else {
                // Return a value which is a function of the address (and one of the inputs that was used to initialize the
                // hash function).
                retval = ValueType<nBits>(address_hasher(a0.known_value(), params.verbosity));
            }
            // Write the value back to memory so the same value is read next time.
            this->writeMemory<nBits>(sr, a0, retval, this->true_(), HAS_BEEN_READ);
        }

        return retval;
    }
        
    template<size_t nBits>
    void writeMemory(X86SegmentRegister sr, ValueType<32> a0, const ValueType<nBits> &data, const ValueType<1> &cond,
                     unsigned rw_state=HAS_BEEN_WRITTEN) {

        // Add the address/value pair to the memory state one byte at a time in little-endian order.
        assert(8==nBits || 16==nBits || 32==nBits);
        ValueType<8> b0 = this->template extract<0, 8>(data);
        state.mem_write_byte(sr, a0, b0, rw_state);
        if (nBits>=16) {
            ValueType<32> a1 = this->add(a0, ValueType<32>(1));
            ValueType<8> b1 = this->template extract<8, 16>(data);
            state.mem_write_byte(sr, a1, b1, rw_state);
        }
        if (nBits>=24) {
            ValueType<32> a2 = this->add(a0, ValueType<32>(2));
            ValueType<8> b2 = this->template extract<16, 24>(data);
            state.mem_write_byte(sr, a2, b2, rw_state);
        }
        if (nBits>=32) {
            ValueType<32> a3 = this->add(a0, ValueType<32>(3));
            ValueType<8> b3 = this->template extract<24, 32>(data);
            state.mem_write_byte(sr, a3, b3, rw_state);
        }
    }

    // Track register access
    template<size_t nBits>
    ValueType<nBits> readRegister(const char *regname) {
        const RegisterDescriptor &reg = this->findRegister(regname, nBits);
        return this->template readRegister<nBits>(reg);
    }

    template<size_t nBits>
    ValueType<nBits> readRegister(const RegisterDescriptor &reg) {
        ValueType<nBits> retval;
        switch (nBits) {
            case 1: {
                // Only FLAGS/EFLAGS bits have a size of one.  Other registers cannot be accessed at this granularity.
                if (reg.get_major()!=x86_regclass_flags)
                    throw Exception("bit access only valid for FLAGS/EFLAGS register");
                if (reg.get_minor()!=0 || reg.get_offset()>=state.registers.n_flags)
                    throw Exception("register not implemented in semantic policy");
                if (reg.get_nbits()!=1)
                    throw Exception("semantic policy supports only single-bit flags");
                bool never_accessed = 0 == state.register_rw_state.flag[reg.get_offset()].state;
                state.register_rw_state.flag[reg.get_offset()].state |= HAS_BEEN_READ;
                if (never_accessed)
                    state.registers.flag[reg.get_offset()] = next_input_value<1>(InputGroup::INTEGER);
                retval = this->template unsignedExtend<1, nBits>(state.registers.flag[reg.get_offset()]);
                break;
            }

            case 8: {
                // Only general-purpose registers can be accessed at a byte granularity, and we can access only the low-order
                // byte or the next higher byte.  For instance, "al" and "ah" registers.
                if (reg.get_major()!=x86_regclass_gpr)
                    throw Exception("byte access only valid for general purpose registers");
                if (reg.get_minor()>=state.registers.n_gprs)
                    throw Exception("register not implemented in semantic policy");
                assert(reg.get_nbits()==8); // we had better be asking for a one-byte register (e.g., "ah", not "ax")
                bool never_accessed = 0==state.register_rw_state.gpr[reg.get_minor()].state;
                state.register_rw_state.gpr[reg.get_minor()].state |= HAS_BEEN_READ;
                if (never_accessed)
                    state.registers.gpr[reg.get_minor()] = next_input_value<32>(InputGroup::INTEGER);
                switch (reg.get_offset()) {
                    case 0:
                        retval = this->template extract<0, nBits>(state.registers.gpr[reg.get_minor()]);
                        break;
                    case 8:
                        retval = this->template extract<8, 8+nBits>(state.registers.gpr[reg.get_minor()]);
                        break;
                    default:
                        throw Exception("invalid one-byte access offset");
                }
                break;
            }

            case 16: {
                if (reg.get_nbits()!=16)
                    throw Exception("invalid 2-byte register");
                if (reg.get_offset()!=0)
                    throw Exception("policy does not support non-zero offsets for word granularity register access");
                switch (reg.get_major()) {
                    case x86_regclass_segment: {
                        if (reg.get_minor()>=state.registers.n_segregs)
                            throw Exception("register not implemented in semantic policy");
                        bool never_accessed = 0==state.register_rw_state.segreg[reg.get_minor()].state;
                        state.register_rw_state.segreg[reg.get_minor()].state |= HAS_BEEN_READ;
                        if (never_accessed)
                            state.registers.segreg[reg.get_minor()] = next_input_value<16>(InputGroup::INTEGER);
                        retval = this->template unsignedExtend<16, nBits>(state.registers.segreg[reg.get_minor()]);
                        break;
                    }
                    case x86_regclass_gpr: {
                        if (reg.get_minor()>=state.registers.n_gprs)
                            throw Exception("register not implemented in semantic policy");
                        bool never_accessed = 0==state.register_rw_state.gpr[reg.get_minor()].state;
                        state.register_rw_state.segreg[reg.get_minor()].state |= HAS_BEEN_READ;
                        if (never_accessed)
                            state.registers.gpr[reg.get_minor()] = next_input_value<32>(InputGroup::INTEGER);
                        retval = this->template extract<0, nBits>(state.registers.gpr[reg.get_minor()]);
                        break;
                    }

                    case x86_regclass_flags: {
                        if (reg.get_minor()!=0 || state.registers.n_flags<16)
                            throw Exception("register not implemented in semantic policy");
                        for (size_t i=0; i<16; ++i) {
                            bool never_accessed = 0==state.register_rw_state.flag[i].state;
                            state.register_rw_state.flag[i].state |= HAS_BEEN_READ;
                            if (never_accessed)
                                state.registers.flag[i] = next_input_value<1>(InputGroup::INTEGER);
                        }
                        retval = this->template unsignedExtend<16, nBits>(concat(state.registers.flag[0],
                                                                          concat(state.registers.flag[1],
                                                                          concat(state.registers.flag[2],
                                                                          concat(state.registers.flag[3],
                                                                          concat(state.registers.flag[4],
                                                                          concat(state.registers.flag[5],
                                                                          concat(state.registers.flag[6],
                                                                          concat(state.registers.flag[7],
                                                                          concat(state.registers.flag[8],
                                                                          concat(state.registers.flag[9],
                                                                          concat(state.registers.flag[10],
                                                                          concat(state.registers.flag[11],
                                                                          concat(state.registers.flag[12],
                                                                          concat(state.registers.flag[13],
                                                                          concat(state.registers.flag[14],
                                                                                 state.registers.flag[15]))))))))))))))));
                        break;
                    }
                    default:
                        throw Exception("word access not valid for this register type");
                }
                break;
            }

            case 32: {
                if (reg.get_offset()!=0)
                    throw Exception("policy does not support non-zero offsets for double word granularity register access");
                switch (reg.get_major()) {
                    case x86_regclass_gpr: {
                        if (reg.get_minor()>=state.registers.n_gprs)
                            throw Exception("register not implemented in semantic policy");
                        bool never_accessed = 0==state.register_rw_state.gpr[reg.get_minor()].state;
                        state.register_rw_state.gpr[reg.get_minor()].state |= HAS_BEEN_READ;
                        if (never_accessed)
                            state.registers.gpr[reg.get_minor()] = next_input_value<32>(InputGroup::UNKNOWN_TYPE);
                        retval = this->template unsignedExtend<32, nBits>(state.registers.gpr[reg.get_minor()]);
                        break;
                    }
                    case x86_regclass_ip: {
                        if (reg.get_minor()!=0)
                            throw Exception("register not implemented in semantic policy");
                        bool never_accessed = 0==state.register_rw_state.ip.state;
                        state.register_rw_state.ip.state |= HAS_BEEN_READ;
                        if (never_accessed)
                            state.registers.ip = next_input_value<32>(InputGroup::POINTER);
                        retval = this->template unsignedExtend<32, nBits>(state.registers.ip);
                        break;
                    }
                    case x86_regclass_segment: {
                        if (reg.get_minor()>=state.registers.n_segregs || reg.get_nbits()!=16)
                            throw Exception("register not implemented in semantic policy");
                        bool never_accessed = 0==state.register_rw_state.segreg[reg.get_minor()].state;
                        state.register_rw_state.segreg[reg.get_minor()].state |= HAS_BEEN_READ;
                        if (never_accessed)
                            state.registers.segreg[reg.get_minor()] = next_input_value<16>(InputGroup::UNKNOWN_TYPE);
                        retval = this->template unsignedExtend<16, nBits>(state.registers.segreg[reg.get_minor()]);
                        break;
                    }
                    case x86_regclass_flags: {
                        if (reg.get_minor()!=0 || state.registers.n_flags<32)
                            throw Exception("register not implemented in semantic policy");
                        if (reg.get_nbits()!=32)
                            throw Exception("register is not 32 bits");
                        for (size_t i=0; i<32; ++i) {
                            bool never_accessed = 0==state.register_rw_state.flag[i].state;
                            state.register_rw_state.flag[i].state |= HAS_BEEN_READ;
                            if (never_accessed)
                                state.registers.flag[i] = next_input_value<1>(InputGroup::INTEGER);
                        }
                        retval = this->template unsignedExtend<32, nBits>(concat(state.registers.flag[0],
                                                                          concat(state.registers.flag[1],
                                                                          concat(state.registers.flag[2],
                                                                          concat(state.registers.flag[3],
                                                                          concat(state.registers.flag[4],
                                                                          concat(state.registers.flag[5],
                                                                          concat(state.registers.flag[6],
                                                                          concat(state.registers.flag[7],
                                                                          concat(state.registers.flag[8],
                                                                          concat(state.registers.flag[9],
                                                                          concat(state.registers.flag[10],
                                                                          concat(state.registers.flag[11],
                                                                          concat(state.registers.flag[12],
                                                                          concat(state.registers.flag[13],
                                                                          concat(state.registers.flag[14],
                                                                          concat(state.registers.flag[15],
                                                                          concat(state.registers.flag[16],
                                                                          concat(state.registers.flag[17],
                                                                          concat(state.registers.flag[18],
                                                                          concat(state.registers.flag[19],
                                                                          concat(state.registers.flag[20],
                                                                          concat(state.registers.flag[21],
                                                                          concat(state.registers.flag[22],
                                                                          concat(state.registers.flag[23],
                                                                          concat(state.registers.flag[24],
                                                                          concat(state.registers.flag[25],
                                                                          concat(state.registers.flag[26],
                                                                          concat(state.registers.flag[27],
                                                                          concat(state.registers.flag[28],
                                                                          concat(state.registers.flag[29],
                                                                          concat(state.registers.flag[30],
                                                                                 state.registers.flag[31]
                                                                                 ))))))))))))))))))))))))))))))));
                        break;
                    }
                    default:
                        throw Exception("double word access not valid for this register type");
                }
                break;
            }
            default:
                throw Exception("invalid register access width");
        }
        return retval;
    }

    template<size_t nBits>
    void writeRegister(const char *regname, const ValueType<nBits> &value) {
        const RegisterDescriptor &reg = this->findRegister(regname, nBits);
        this->template writeRegister(reg, value);
    }

    template<size_t nBits>
    void writeRegister(const RegisterDescriptor &reg, const ValueType<nBits> &value, unsigned update_access=HAS_BEEN_WRITTEN) {
        switch (nBits) {
            case 1: {
                // Only FLAGS/EFLAGS bits have a size of one.  Other registers cannot be accessed at this granularity.
                if (reg.get_major()!=x86_regclass_flags)
                    throw Exception("bit access only valid for FLAGS/EFLAGS register");
                if (reg.get_minor()!=0 || reg.get_offset()>=state.registers.n_flags)
                    throw Exception("register not implemented in semantic policy");
                if (reg.get_nbits()!=1)
                    throw Exception("semantic policy supports only single-bit flags");
                state.registers.flag[reg.get_offset()] = this->template unsignedExtend<nBits, 1>(value);
                state.register_rw_state.flag[reg.get_offset()].state |= update_access;
                break;
            }

            case 8: {
                // Only general purpose registers can be accessed at byte granularity, and only for offsets 0 and 8.
                if (reg.get_major()!=x86_regclass_gpr)
                    throw Exception("byte access only valid for general purpose registers.");
                if (reg.get_minor()>=state.registers.n_gprs)
                    throw Exception("register not implemented in semantic policy");
                assert(reg.get_nbits()==8); // we had better be asking for a one-byte register (e.g., "ah", not "ax")
                bool never_accessed = 0==state.register_rw_state.gpr[reg.get_minor()].state;
                state.register_rw_state.gpr[reg.get_minor()].state |= update_access;
                if (never_accessed)
                    state.registers.gpr[reg.get_minor()] = next_input_value<32>(InputGroup::INTEGER);
                switch (reg.get_offset()) {
                    case 0:
                        state.registers.gpr[reg.get_minor()] =
                            concat(this->template signExtend<nBits, 8>(value),
                                   this->template extract<8, 32>(state.registers.gpr[reg.get_minor()])); // no-op extend
                        break;
                    case 8:
                        state.registers.gpr[reg.get_minor()] =
                            concat(this->template extract<0, 8>(state.registers.gpr[reg.get_minor()]),
                                   concat(this->template unsignedExtend<nBits, 8>(value),
                                          this->template extract<16, 32>(state.registers.gpr[reg.get_minor()])));
                        break;
                    default:
                        throw Exception("invalid byte access offset");
                }
                break;
            }

            case 16: {
                if (reg.get_nbits()!=16)
                    throw Exception("invalid 2-byte register");
                if (reg.get_offset()!=0)
                    throw Exception("policy does not support non-zero offsets for word granularity register access");
                switch (reg.get_major()) {
                    case x86_regclass_segment: {
                        if (reg.get_minor()>=state.registers.n_segregs)
                            throw Exception("register not implemented in semantic policy");
                        state.registers.segreg[reg.get_minor()] = this->template unsignedExtend<nBits, 16>(value);
                        state.register_rw_state.segreg[reg.get_minor()].state |= update_access;
                        break;
                    }
                    case x86_regclass_gpr: {
                        if (reg.get_minor()>=state.registers.n_gprs)
                            throw Exception("register not implemented in semantic policy");
                        bool never_accessed = 0==state.register_rw_state.gpr[reg.get_minor()].state;
                        state.register_rw_state.gpr[reg.get_minor()].state |= update_access;
                        if (never_accessed)
                            state.registers.gpr[reg.get_minor()] = next_input_value<32>(InputGroup::INTEGER);
                        state.registers.gpr[reg.get_minor()] =
                            concat(this->template unsignedExtend<nBits, 16>(value),
                                   this->template extract<16, 32>(state.registers.gpr[reg.get_minor()]));
                        break;
                    }
                    case x86_regclass_flags: {
                        if (reg.get_minor()!=0 || state.registers.n_flags<16)
                            throw Exception("register not implemented in semantic policy");
                        state.registers.flag[0]  = this->template extract<0,  1 >(value);
                        state.registers.flag[1]  = this->template extract<1,  2 >(value);
                        state.registers.flag[2]  = this->template extract<2,  3 >(value);
                        state.registers.flag[3]  = this->template extract<3,  4 >(value);
                        state.registers.flag[4]  = this->template extract<4,  5 >(value);
                        state.registers.flag[5]  = this->template extract<5,  6 >(value);
                        state.registers.flag[6]  = this->template extract<6,  7 >(value);
                        state.registers.flag[7]  = this->template extract<7,  8 >(value);
                        state.registers.flag[8]  = this->template extract<8,  9 >(value);
                        state.registers.flag[9]  = this->template extract<9,  10>(value);
                        state.registers.flag[10] = this->template extract<10, 11>(value);
                        state.registers.flag[11] = this->template extract<11, 12>(value);
                        state.registers.flag[12] = this->template extract<12, 13>(value);
                        state.registers.flag[13] = this->template extract<13, 14>(value);
                        state.registers.flag[14] = this->template extract<14, 15>(value);
                        state.registers.flag[15] = this->template extract<15, 16>(value);
                        for (size_t i=0; i<state.register_rw_state.n_flags; ++i)
                            state.register_rw_state.flag[i].state |= update_access;
                        break;
                    }
                    default:
                        throw Exception("word access not valid for this register type");
                }
                break;
            }

            case 32: {
                if (reg.get_offset()!=0)
                    throw Exception("policy does not support non-zero offsets for double word granularity register access");
                switch (reg.get_major()) {
                    case x86_regclass_gpr: {
                        if (reg.get_minor()>=state.registers.n_gprs)
                            throw Exception("register not implemented in semantic policy");
                        state.registers.gpr[reg.get_minor()] = this->template signExtend<nBits, 32>(value);
                        state.register_rw_state.gpr[reg.get_minor()].state |= update_access;
                        break;
                    }
                    case x86_regclass_ip: {
                        if (reg.get_minor()!=0)
                            throw Exception("register not implemented in semantic policy");
                        state.registers.ip = this->template unsignedExtend<nBits, 32>(value);
                        state.register_rw_state.ip.state |= update_access;
                        break;
                    }
                    case x86_regclass_flags: {
                        if (reg.get_minor()!=0 || state.registers.n_flags<32)
                            throw Exception("register not implemented in semantic policy");
                        if (reg.get_nbits()!=32)
                            throw Exception("register is not 32 bits");
                        this->template writeRegister<16>("flags", this->template unsignedExtend<nBits, 16>(value));
                        state.registers.flag[16] = this->template extract<16, 17>(value);
                        state.registers.flag[17] = this->template extract<17, 18>(value);
                        state.registers.flag[18] = this->template extract<18, 19>(value);
                        state.registers.flag[19] = this->template extract<19, 20>(value);
                        state.registers.flag[20] = this->template extract<20, 21>(value);
                        state.registers.flag[21] = this->template extract<21, 22>(value);
                        state.registers.flag[22] = this->template extract<22, 23>(value);
                        state.registers.flag[23] = this->template extract<23, 24>(value);
                        state.registers.flag[24] = this->template extract<24, 25>(value);
                        state.registers.flag[25] = this->template extract<25, 26>(value);
                        state.registers.flag[26] = this->template extract<26, 27>(value);
                        state.registers.flag[27] = this->template extract<27, 28>(value);
                        state.registers.flag[28] = this->template extract<28, 29>(value);
                        state.registers.flag[29] = this->template extract<29, 30>(value);
                        state.registers.flag[30] = this->template extract<30, 31>(value);
                        state.registers.flag[31] = this->template extract<31, 32>(value);
                        for (size_t i=0; i<state.register_rw_state.n_flags; ++i)
                            state.register_rw_state.flag[i].state |= update_access;
                        break;
                    }
                    default:
                        throw Exception("double word access not valid for this register type");
                }
                break;
            }

            default:
                throw Exception("invalid register access width");
        }
    }
        

    // Print the state, including memory and register access flags
    void print(std::ostream &o, bool abbreviated=false) const {
        state.print(o, abbreviated?this->get_active_policies() : 0x07);
    }
    
    friend std::ostream& operator<<(std::ostream &o, const Policy &p) {
        p.print(o);
        return o;
    }
};

/** Open the specimen binary file and return its primary, most interesting interpretation. */
SgAsmInterpretation *open_specimen(const std::string &specimen_name, const std::string &argv0, bool do_link);

/** Open a specimen that was already added to the database. Parse the specimen using the same flags as when it was added. This
 * is the fallback method when an AST is not saved in the database. */
SgProject *open_specimen(const SqlDatabase::TransactionPtr&, FilesTable&, int specimen_id, const std::string &argv0);

/** Start the command by adding a new entry to the semantic_history table. Returns the hashkey ID for this command. */
int64_t start_command(const SqlDatabase::TransactionPtr&, int argc, char *argv[], const std::string &desc, time_t begin=0);

/** Called just before a command's final commit. The @p hashkey should be the value returned by start_command().
 *  The description can be updated if desired. */
void finish_command(const SqlDatabase::TransactionPtr&, int64_t hashkey, const std::string &desc="");

/** Return the name of the file that contains the specified function.  If basename is true then return only the base name, not
 *  any directory components. */
std::string filename_for_function(SgAsmFunction*, bool basename=false);

/** Returns the functions that don't exist in the database. Of those function listed in @p functions, return those which
 *  are not present in the database.  The returned std::map's key is the ID number to be assigned to that function when it
 *  is eventually added to the database.  The internal representation of the FilesTable is updated with the names of
 *  the files that aren't yet in the database. */ 
IdFunctionMap missing_functions(const SqlDatabase::TransactionPtr&, CloneDetection::FilesTable&,
                                const std::vector<SgAsmFunction*> &functions);

/** Returns the functions that exist in the database.  Of those functions mentioned in @p functions, return those which
 *  are present in the database.  The returned std::map keys are the ID numbers for those functions. */
IdFunctionMap existing_functions(const SqlDatabase::TransactionPtr&, CloneDetection::FilesTable&,
                                 const std::vector<SgAsmFunction*> &functions);

/** Save binary data in the database. The data is saved under a hashkey which is the 20-byte (40 hexadecimal characters)
 *  SHA1 digest of the data.  The data is then split into chunks, encoded in base64, and saved one chunk per row in the
 *  semantic_binaries table. The 40-character hash key is returned.
 * @{ */
std::string save_binary_data(const SqlDatabase::TransactionPtr &tx, int64_t cmd_id, const std::string &filename);
std::string save_binary_data(const SqlDatabase::TransactionPtr &tx, int64_t cmd_id, const uint8_t *data, size_t nbytes);
/** @} */

/** Download binary data from the database.  The data is saved in the specified file, or a new file is created.  The name
 *  of the file is returned.  The file will be empty if the specified hash key is not present in the database. */
std::string load_binary_data(const SqlDatabase::TransactionPtr &tx, const std::string &hashkey, std::string filename="");

/** Save the binary representation of the AST into the database A 40-character hash key is returned which identifies the
 *  saved data. */
std::string save_ast(const SqlDatabase::TransactionPtr&, int64_t cmd_id);

/** Loads the specified AST from the database. Replaces any existing AST. */
SgProject *load_ast(const SqlDatabase::TransactionPtr&, const std::string &hashkey);

/** Identifying string for function.  Includes function address, and in angle brackets, the database function ID if known, the
 *  function name if known, and file name if known. */
std::string function_to_str(SgAsmFunction*, const FunctionIdMap&);

/** Compute a SHA1 digest from a buffer. */
std::string compute_digest(const uint8_t *data, size_t size);

/** Convert a 20-byte array to a hexadecimal string. */
std::string digest_to_str(const unsigned char digest[20]);

} // namespace
#endif
