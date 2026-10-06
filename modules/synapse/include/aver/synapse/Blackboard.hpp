#pragma once
// Typed blackboard: named, typed keys with per-agent and shared (team) scopes, change stamps and
// change observers. Pure (no scene, no world) so it tests with plain C++. Design: docs/BLACKBOARD_BT.md.
#include "aver/core/Math.hpp"
#include "aver/core/Types.hpp"

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace aver::synapse {

enum class BbType : u8 { Bool = 0, Int = 1, Float = 2, Vec3 = 3, String = 4, Entity = 5 };
enum class BbScope : u8 { Agent = 0, Shared = 1 };

// Comparison used by decorators and the BbCompare leaf. IsSet/NotSet test truthiness and ignore the operand.
enum class BbOp : u8 { Eq = 0, Ne = 1, Lt = 2, Le = 3, Gt = 4, Ge = 5, IsSet = 6, NotSet = 7 };

struct BbValue {
    BbType type = BbType::Int;
    i64 i = 0;          // Bool, Int, Entity
    f32 f = 0.0f;       // Float
    Vec3 v;             // Vec3
    std::string s;      // String

    static BbValue ofBool(bool b)               { BbValue r; r.type = BbType::Bool;   r.i = b ? 1 : 0; return r; }
    static BbValue ofInt(i64 n)                 { BbValue r; r.type = BbType::Int;    r.i = n; return r; }
    static BbValue ofFloat(f32 x)               { BbValue r; r.type = BbType::Float;  r.f = x; return r; }
    static BbValue ofVec3(const Vec3& p)        { BbValue r; r.type = BbType::Vec3;   r.v = p; return r; }
    static BbValue ofString(std::string text)   { BbValue r; r.type = BbType::String; r.s = std::move(text); return r; }
    static BbValue ofEntity(u32 e)              { BbValue r; r.type = BbType::Entity; r.i = static_cast<i64>(e); return r; }
};

// Exact equality (NaN equals NaN, so a NaN written twice is not a change).
bool operator==(const BbValue& a, const BbValue& b);
inline bool operator!=(const BbValue& a, const BbValue& b) { return !(a == b); }

const char* bbTypeName(BbType t);
bool        bbTypeFromName(std::string_view name, BbType& out);
const char* bbOpName(BbOp op);
bool        bbOpFromName(std::string_view name, BbOp& out);
const char* bbScopeName(BbScope s);

BbValue     bbDefault(BbType t);
// Numeric conversions between Bool/Int/Float/Entity; Vec3 and String only convert to themselves.
bool        bbCoerce(BbType target, const BbValue& in, BbValue& out);
std::string bbToString(const BbValue& v);
// Parses text for `type`: true/false/1/0, integers, floats, "x,y,z", raw string.
bool        bbParse(BbType type, std::string_view text, BbValue& out);
// Truthiness used by IsSet/NotSet.
bool        bbTruthy(const BbValue& v);
// actual <op> expected; expected is coerced to actual's type first. Unsupported combinations are false.
bool        bbCompare(const BbValue& actual, BbOp op, const BbValue& expected);

struct BbKeyDef {
    std::string name;
    BbType      type  = BbType::Int;
    BbScope     scope = BbScope::Agent;
    BbValue     defaultValue;       // type is forced to `type` on define
    std::string description;
};

struct BbSchema {
    std::vector<BbKeyDef> keys;

    i32  indexOf(std::string_view name) const;
    const BbKeyDef* find(std::string_view name) const;
    // False on an empty/duplicate name.
    bool add(BbKeyDef def);
    bool remove(std::string_view name);
};

// "key", "!key", "key op value". The value is typed by `type` when the key is known, else guessed.
struct BbExpr {
    std::string key;
    BbOp        op = BbOp::IsSet;
    BbValue     value;
};
bool bbParseExpr(std::string_view text, const BbSchema* schema, BbExpr& out, std::string* why = nullptr);
std::string bbFormatExpr(const BbExpr& e);

class Blackboard {
public:
    using ObserverId = u32;
    using Observer   = std::function<void(std::string_view key, const BbValue& oldValue, const BbValue& newValue)>;

    Blackboard() = default;
    ~Blackboard();
    Blackboard(const Blackboard&) = delete;
    Blackboard& operator=(const Blackboard&) = delete;

    // The team board that Shared-scope keys live in. Existing Shared keys migrate (the team board's
    // own value wins when it already has the key). nullptr detaches: Shared keys become local copies.
    void        setShared(Blackboard* shared);
    Blackboard* shared() const { return shared_; }

    // Adds a key. Same name and type keeps the existing value and returns its index; a different type
    // returns -1. Shared keys on a board with a team board are defined there too.
    i32  defineKey(const BbKeyDef& def);
    void applySchema(const BbSchema& schema);
    bool removeKey(std::string_view name);

    i32   indexOf(std::string_view name) const;
    bool  has(std::string_view name) const { return indexOf(name) >= 0; }
    usize size() const { return slots_.size(); }
    const BbKeyDef& keyDef(i32 index) const { return slots_[static_cast<usize>(index)].def; }
    const BbValue&  valueAt(i32 index) const;
    u64             stampAt(i32 index) const;

    const BbValue* get(std::string_view name) const;
    // Accepts the value (coerced to the key's type); false on an undefined key or a type that cannot
    // convert. Observers fire only when the stored value actually changes.
    bool set(std::string_view name, const BbValue& value);
    bool reset(std::string_view name);
    // Change stamp from a process-wide counter; 0 when the key is undefined. Comparable across boards.
    u64  stamp(std::string_view name) const;

    bool getBool(std::string_view name, bool fallback = false) const;
    i64  getInt(std::string_view name, i64 fallback = 0) const;
    f32  getFloat(std::string_view name, f32 fallback = 0.0f) const;
    Vec3 getVec3(std::string_view name, const Vec3& fallback = Vec3{}) const;
    std::string getString(std::string_view name, const std::string& fallback = {}) const;
    u32  getEntity(std::string_view name, u32 fallback = 0) const;
    bool setBool(std::string_view name, bool v)               { return set(name, BbValue::ofBool(v)); }
    bool setInt(std::string_view name, i64 v)                 { return set(name, BbValue::ofInt(v)); }
    bool setFloat(std::string_view name, f32 v)               { return set(name, BbValue::ofFloat(v)); }
    bool setVec3(std::string_view name, const Vec3& v)        { return set(name, BbValue::ofVec3(v)); }
    bool setString(std::string_view name, std::string v)      { return set(name, BbValue::ofString(std::move(v))); }
    bool setEntity(std::string_view name, u32 v)              { return set(name, BbValue::ofEntity(v)); }

    // Observer for one key ("" = every key stored on THIS board). A Shared key observed through an
    // agent board registers on the team board, so it fires for any agent's write. Callbacks run
    // synchronously inside set(); they may set other keys or unobserve.
    ObserverId observe(std::string_view key, Observer fn);
    void       unobserve(ObserverId id);

private:
    struct Slot {
        BbKeyDef def;
        BbValue  value;
        u64      stamp = 0;
        bool     forwarded = false;   // value lives on shared_
    };
    struct ObserverRec { ObserverId id = 0; std::string key; Observer fn; };
    struct Handle {
        ObserverId  id = 0;
        std::string key;
        Observer    fn;
        Blackboard* target = nullptr;     // the board that holds the live record
        ObserverId  targetId = 0;
    };

    Slot*       slot(std::string_view name);
    const Slot* slot(std::string_view name) const;
    void        notify(std::string_view key, const BbValue& oldV, const BbValue& newV);
    bool        setLocal(Slot& s, const BbValue& coerced);
    ObserverId  addLocal(std::string_view key, Observer fn);
    void        removeLocal(ObserverId id);
    void        rebindObservers();

    Blackboard* shared_ = nullptr;
    std::vector<Slot> slots_;
    std::unordered_map<std::string, i32> index_;
    std::vector<ObserverRec> observers_;
    std::vector<Handle> handles_;
    ObserverId nextObserver_ = 1;
};

// Same as bbParseExpr, typing the value from a live board's key.
bool bbParseExprFor(std::string_view text, const Blackboard& board, BbExpr& out, std::string* why = nullptr);

// Team boards, created on demand. id 0 is the default shared board.
class SharedBlackboards {
public:
    static u64 idOf(std::string_view team);   // 0 for ""
    Blackboard& get(u64 id);
    Blackboard* find(u64 id);
    void clear() { boards_.clear(); }
    usize count() const { return boards_.size(); }

private:
    std::unordered_map<u64, std::unique_ptr<Blackboard>> boards_;
};

} // namespace aver::synapse
