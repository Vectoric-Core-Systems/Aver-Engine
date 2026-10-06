// Typed blackboard: types and coercion, change stamps, observers, and agent/team scope isolation.
// Pure: no scene, no world.
#include "aver/synapse/Blackboard.hpp"

#include "aver/core/Log.hpp"

#include <cmath>
#include <string>
#include <vector>

using namespace aver;
using namespace aver::synapse;

static int g_failures = 0;

static void check(bool cond, const std::string& what) {
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {}", what);
}

static BbKeyDef key(const char* name, BbType t, BbScope s = BbScope::Agent) {
    BbKeyDef d;
    d.name = name;
    d.type = t;
    d.scope = s;
    return d;
}

int main() {
    AVER_INFO("BlackboardTest");

    {
        AVER_INFO("typed storage, defaults and coercion");
        Blackboard b;
        BbKeyDef hp = key("Health", BbType::Float);
        hp.defaultValue = BbValue::ofFloat(100.0f);
        check(b.defineKey(hp) == 0, "the first key gets index 0");
        b.defineKey(key("Alert", BbType::Bool));
        b.defineKey(key("Count", BbType::Int));
        b.defineKey(key("Home", BbType::Vec3));
        b.defineKey(key("Name", BbType::String));
        b.defineKey(key("Foe", BbType::Entity));

        check(b.getFloat("Health") == 100.0f, "a key starts at its default");
        check(b.setInt("Health", 40) && b.getFloat("Health") == 40.0f, "an Int written to a Float key converts");
        check(b.setFloat("Count", 7.9f) && b.getInt("Count") == 7, "a Float written to an Int key truncates");
        check(b.setInt("Alert", 5) && b.getBool("Alert"), "a non-zero Int written to a Bool key is true");
        check(!b.setString("Count", "x"), "a String cannot be written to an Int key");
        check(b.getInt("Count") == 7, "and the value is untouched after the refusal");
        check(!b.setVec3("Health", Vec3{1, 2, 3}), "a Vec3 cannot be written to a Float key");
        check(!b.setFloat("Missing", 1.0f), "writing an undefined key fails");
        check(b.get("Missing") == nullptr, "reading an undefined key gives null");
        check(b.getInt("Missing", 42) == 42, "typed getters return the fallback for an undefined key");
        check(b.setVec3("Home", Vec3{1, 2, 3}) && b.getVec3("Home").y == 2.0f, "Vec3 round trips");
        check(b.setEntity("Foe", 12) && b.getEntity("Foe") == 12, "Entity round trips");
        check(b.setString("Name", "guard") && b.getString("Name") == "guard", "String round trips");
        check(b.reset("Health") && b.getFloat("Health") == 100.0f, "reset restores the default");

        check(b.defineKey(key("Health", BbType::Int)) == -1, "re-defining a key with another type is refused");
        check(b.defineKey(key("Health", BbType::Float)) == 0, "re-defining with the same type keeps the key");
        check(b.getFloat("Health") == 100.0f, "and keeps its value");
    }

    {
        AVER_INFO("change stamps and observers");
        Blackboard b;
        b.defineKey(key("A", BbType::Int));
        b.defineKey(key("B", BbType::Int));

        const u64 s0 = b.stamp("A");
        check(s0 != 0, "a defined key has a stamp");
        b.setInt("A", 0);
        check(b.stamp("A") == s0, "writing the same value is not a change");
        b.setInt("A", 1);
        check(b.stamp("A") > s0, "a real change bumps the stamp");
        check(b.stamp("Nope") == 0, "an undefined key has stamp 0");

        std::vector<std::string> log;
        const auto idA = b.observe("A", [&](std::string_view k, const BbValue& o, const BbValue& n) {
            log.push_back(std::string(k) + ":" + bbToString(o) + "->" + bbToString(n));
        });
        int anyCount = 0;
        b.observe("", [&](std::string_view, const BbValue&, const BbValue&) { ++anyCount; });

        b.setInt("A", 2);
        b.setInt("A", 2);
        b.setInt("B", 9);
        check(log.size() == 1 && log[0] == "A:1->2", "a key observer fires once per real change with old and new");
        check(anyCount == 2, "the wildcard observer sees changes to every key");
        b.unobserve(idA);
        b.setInt("A", 3);
        check(log.size() == 1, "an unobserved key stops notifying");

        Blackboard c;
        c.defineKey(key("In", BbType::Int));
        c.defineKey(key("Out", BbType::Int));
        c.observe("In", [&](std::string_view, const BbValue&, const BbValue& n) { c.setInt("Out", n.i * 2); });
        c.setInt("In", 21);
        check(c.getInt("Out") == 42, "an observer may write other keys");

        Blackboard d;
        d.defineKey(key("F", BbType::Float));
        int fires = 0;
        d.observe("F", [&](std::string_view, const BbValue&, const BbValue&) { ++fires; });
        d.setFloat("F", std::nanf(""));
        d.setFloat("F", std::nanf(""));
        check(fires == 1, "NaN written twice is one change, not two");
    }

    {
        AVER_INFO("scope isolation");
        SharedBlackboards teams;
        BbSchema schema;
        schema.add(key("Ammo", BbType::Int));
        schema.add(key("Alert", BbType::Bool, BbScope::Shared));

        Blackboard a1, a2, b1;
        Blackboard& red = teams.get(SharedBlackboards::idOf("red"));
        Blackboard& blue = teams.get(SharedBlackboards::idOf("blue"));
        a1.setShared(&red);
        a2.setShared(&red);
        b1.setShared(&blue);
        a1.applySchema(schema);
        a2.applySchema(schema);
        b1.applySchema(schema);

        a1.setInt("Ammo", 5);
        check(a2.getInt("Ammo") == 0 && b1.getInt("Ammo") == 0, "Agent keys are private to one board");

        a1.setBool("Alert", true);
        check(a2.getBool("Alert"), "a Shared key written by one agent is seen by its teammate");
        check(!b1.getBool("Alert"), "and is not seen by another team");
        check(red.getBool("Alert") && !blue.getBool("Alert"), "the team boards hold the values");
        check(a1.stamp("Alert") == a2.stamp("Alert"), "teammates see the same change stamp for a Shared key");

        int fired = 0;
        a2.observe("Alert", [&](std::string_view, const BbValue&, const BbValue&) { ++fired; });
        a1.setBool("Alert", false);
        check(fired == 1, "an observer on a Shared key fires for another agent's write");

        Blackboard loner;
        loner.applySchema(schema);
        loner.setBool("Alert", true);
        check(!a1.getBool("Alert") && !a2.getBool("Alert"), "a board without a team does not touch the team boards");

        loner.setShared(&blue);
        check(!loner.getBool("Alert"), "joining a team: the team board's own value wins when it already has the key");
        loner.setBool("Alert", true);
        check(b1.getBool("Alert"), "after joining, writes reach the team");

        loner.setShared(nullptr);
        check(loner.getBool("Alert"), "leaving a team keeps the last value as a local copy");
        loner.setBool("Alert", false);
        check(b1.getBool("Alert"), "and later writes stay local");

        Blackboard mover;
        mover.defineKey(key("Alert", BbType::Bool, BbScope::Shared));
        int moverFires = 0;
        mover.observe("Alert", [&](std::string_view, const BbValue&, const BbValue&) { ++moverFires; });
        mover.setShared(&red);
        red.setBool("Alert", true);
        check(moverFires == 1, "an observer follows its key onto the team board");

        Blackboard clash;
        clash.setShared(&red);
        check(clash.defineKey(key("Alert", BbType::Int, BbScope::Shared)) == -1,
              "a Shared key whose type disagrees with the team's is refused");
    }

    {
        AVER_INFO("a destroyed agent board unregisters from the team board");
        SharedBlackboards teams;
        Blackboard& t = teams.get(1);
        int fired = 0;
        {
            Blackboard agent;
            agent.setShared(&t);
            agent.defineKey(key("X", BbType::Int, BbScope::Shared));
            agent.observe("X", [&](std::string_view, const BbValue&, const BbValue&) { ++fired; });
            t.setInt("X", 1);
        }
        t.setInt("X", 2);
        check(fired == 1, "no callback after the observing agent is gone");
    }

    {
        AVER_INFO("expressions and comparisons");
        BbSchema schema;
        schema.add(key("Dist", BbType::Float));
        schema.add(key("Flag", BbType::Bool));
        schema.add(key("Label", BbType::String));
        schema.add(key("Pos", BbType::Vec3));

        BbExpr e;
        std::string why;
        check(bbParseExpr("Dist < 500", &schema, e, &why) && e.key == "Dist" && e.op == BbOp::Lt && e.value.f == 500.0f,
              "key < number");
        check(bbParseExpr("Flag == true", &schema, e) && e.op == BbOp::Eq && e.value.i == 1, "key == bool");
        check(bbParseExpr("!Flag", &schema, e) && e.op == BbOp::NotSet && e.key == "Flag", "!key is NotSet");
        check(bbParseExpr("Flag", &schema, e) && e.op == BbOp::IsSet, "a bare key is IsSet");
        check(bbParseExpr("Pos = 1, 2, 3", &schema, e) && e.op == BbOp::Eq && e.value.v.z == 3.0f,
              "= reads as ==, with a vec3 value");
        check(bbParseExpr("Label != hello world", &schema, e) && e.value.s == "hello world", "string values keep spaces");
        check(!bbParseExpr("Dist < banana", &schema, e), "a value that does not parse as the key's type is refused");
        check(!bbParseExpr("", &schema, e), "an empty expression is refused");

        BbExpr rt;
        bbParseExpr("Dist >= 12.5", &schema, rt);
        BbExpr again;
        check(bbParseExpr(bbFormatExpr(rt), &schema, again) && again.op == rt.op && again.value == rt.value,
              "format then parse is stable");

        Blackboard board;
        board.applySchema(schema);
        check(bbParseExprFor("Dist > 3", board, e) && e.value.type == BbType::Float, "a live board types the value");

        check(bbCompare(BbValue::ofInt(3), BbOp::Lt, BbValue::ofFloat(3.5f)), "Int < Float compares numerically");
        check(bbCompare(BbValue::ofString("a"), BbOp::Lt, BbValue::ofString("b")), "strings order lexicographically");
        check(!bbCompare(BbValue::ofVec3(Vec3{}), BbOp::Lt, BbValue::ofVec3(Vec3{})), "Vec3 has no ordering");
        check(bbCompare(BbValue::ofVec3(Vec3{1, 2, 3}), BbOp::Eq, BbValue::ofVec3(Vec3{1, 2, 3})), "Vec3 equality");
        check(bbCompare(BbValue::ofString(""), BbOp::NotSet, BbValue{}), "an empty string is not set");
        check(bbCompare(BbValue::ofEntity(4), BbOp::IsSet, BbValue{}), "a non-zero entity is set");
        check(bbCompare(BbValue::ofString("x"), BbOp::Ne, BbValue::ofInt(1)), "mismatched kinds are never equal");
    }

    AVER_INFO(g_failures ? "BlackboardTest: {} FAILURES" : "BlackboardTest: all checks passed ({})", g_failures);
    return g_failures ? 1 : 0;
}
