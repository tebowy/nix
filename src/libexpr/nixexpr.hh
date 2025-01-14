#pragma once
///@file

#include <map>
#include <vector>

#include "value.hh"
#include "symbol-table.hh"
#include "error.hh"
#include "position.hh"
#include "eval-error.hh"
#include "pos-idx.hh"
#include "pos-table.hh"

namespace nix {


struct Env;
struct Value;
class EvalState;
struct ExprWith;
struct StaticEnv;


/**
 * An attribute path is a sequence of attribute names.
 */
struct AttrName
{
    Symbol symbol;
    std::unique_ptr<Expr> expr;
    AttrName(Symbol s) : symbol(s) {};
    AttrName(std::unique_ptr<Expr> e) : expr(std::move(e)) {};
};

typedef std::vector<AttrName> AttrPath;

std::string showAttrPath(const SymbolTable & symbols, const AttrPath & attrPath);


/* Abstract syntax of Nix expressions. */

struct Expr
{
protected:
    Expr(Expr &&) = default;
    Expr & operator=(Expr &&) = default;

public:
    struct AstSymbols {
        Symbol sub, lessThan, mul, div, or_, findFile, nixPath, body;
    };


    Expr() = default;
    Expr(const Expr &) = delete;
    Expr & operator=(const Expr &) = delete;
    virtual ~Expr() { };

    virtual void show(const SymbolTable & symbols, std::ostream & str) const;
    virtual void bindVars(EvalState & es, const std::shared_ptr<const StaticEnv> & env);
    virtual void eval(EvalState & state, Env & env, Value & v);
    virtual Value * maybeThunk(EvalState & state, Env & env);
    virtual void setName(Symbol name);
    virtual PosIdx getPos() const { return noPos; }
};

#define COMMON_METHODS \
    void show(const SymbolTable & symbols, std::ostream & str) const override; \
    void eval(EvalState & state, Env & env, Value & v) override; \
    void bindVars(EvalState & es, const std::shared_ptr<const StaticEnv> & env) override;

struct ExprInt : Expr
{
    NixInt n;
    Value v;
    ExprInt(NixInt n) : n(n) { v.mkInt(n); };
    Value * maybeThunk(EvalState & state, Env & env) override;
    COMMON_METHODS
};

struct ExprFloat : Expr
{
    NixFloat nf;
    Value v;
    ExprFloat(NixFloat nf) : nf(nf) { v.mkFloat(nf); };
    Value * maybeThunk(EvalState & state, Env & env) override;
    COMMON_METHODS
};

struct ExprString : Expr
{
    std::string s;
    Value v;
    ExprString(std::string &&s) : s(std::move(s)) { v.mkString(this->s.data()); };
    Value * maybeThunk(EvalState & state, Env & env) override;
    COMMON_METHODS
};

struct ExprPath : Expr
{
    std::string s;
    Value v;
    ExprPath(std::string s) : s(std::move(s)) { v.mkPath(this->s.c_str()); };
    Value * maybeThunk(EvalState & state, Env & env) override;
    COMMON_METHODS
};

typedef uint32_t Level;
typedef uint32_t Displacement;

struct ExprVar : Expr
{
    PosIdx pos;
    Symbol name;

    /* Whether the variable comes from an environment (e.g. a rec, let
       or function argument) or from a "with".

       `nullptr`: Not from a `with`.
       Valid pointer: the nearest, innermost `with` expression to query first. */
    ExprWith * fromWith;

    /* In the former case, the value is obtained by going `level`
       levels up from the current environment and getting the
       `displ`th value in that environment.  In the latter case, the
       value is obtained by getting the attribute named `name` from
       the set stored in the environment that is `level` levels up
       from the current one.*/
    Level level;
    Displacement displ;

    ExprVar(Symbol name) : name(name) { };
    ExprVar(const PosIdx & pos, Symbol name) : pos(pos), name(name) { };
    Value * maybeThunk(EvalState & state, Env & env) override;
    PosIdx getPos() const override { return pos; }
    COMMON_METHODS
};

/**
 * A pseudo-expression for the purpose of evaluating the `from` expression in `inherit (from)` syntax.
 * Unlike normal variable references, the displacement is set during parsing, and always refers to
 * `ExprAttrs::inheritFromExprs` (by itself or in `ExprLet`), whose values are put into their own `Env`.
 */
struct ExprInheritFrom : ExprVar
{
    ref<Expr> fromExpr;

    ExprInheritFrom(PosIdx pos, Displacement displ, ref<Expr> fromExpr)
          : ExprVar(pos, {}), fromExpr(fromExpr)
    {
        this->level = 0;
        this->displ = displ;
        this->fromWith = nullptr;
    }

    void show(SymbolTable const & symbols, std::ostream & str) const override;
    void bindVars(EvalState & es, const std::shared_ptr<const StaticEnv> & env) override;
};

struct ExprSelect : Expr
{
    PosIdx pos;

    /** The expression attributes are being selected on. e.g. `foo` in `foo.bar.baz`. */
    std::unique_ptr<Expr> e;

    /** A default value specified with `or`, if the selected attr doesn't exist.
     * e.g. `bix` in `foo.bar.baz or bix`
     */
    std::unique_ptr<Expr> def;

    /** The path of attributes being selected. e.g. `bar.baz` in `foo.bar.baz.` */
    AttrPath attrPath;

    ExprSelect(const PosIdx & pos, std::unique_ptr<Expr> e, AttrPath attrPath, std::unique_ptr<Expr> def) : pos(pos), e(std::move(e)), def(std::move(def)), attrPath(std::move(attrPath)) { };
    ExprSelect(const PosIdx & pos, std::unique_ptr<Expr> e, Symbol name) : pos(pos), e(std::move(e)) { attrPath.push_back(AttrName(name)); };
    PosIdx getPos() const override { return pos; }
    COMMON_METHODS
};

struct ExprOpHasAttr : Expr
{
    std::unique_ptr<Expr> e;
    AttrPath attrPath;
    ExprOpHasAttr(std::unique_ptr<Expr> e, AttrPath attrPath) : e(std::move(e)), attrPath(std::move(attrPath)) { };
    PosIdx getPos() const override { return e->getPos(); }
    COMMON_METHODS
};

struct ExprAttrs : Expr
{
    bool recursive;
    PosIdx pos;
    struct AttrDef {
        enum class Kind {
            /** `attr = expr;` */
            Plain,
            /** `inherit attr1 attrn;` */
            Inherited,
            /** `inherit (expr) attr1 attrn;` */
            InheritedFrom,
        };

        Kind kind;
        std::unique_ptr<Expr> e;
        PosIdx pos;
        Displacement displ; // displacement
        AttrDef(std::unique_ptr<Expr> e, const PosIdx & pos, Kind kind = Kind::Plain)
            : kind(kind), e(std::move(e)), pos(pos) { };
        AttrDef() { };

        template<typename T>
        const T & chooseByKind(const T & plain, const T & inherited, const T & inheritedFrom) const
        {
            switch (kind) {
            case Kind::Plain:
                return plain;
            case Kind::Inherited:
                return inherited;
            default:
            case Kind::InheritedFrom:
                return inheritedFrom;
            }
        }
    };
    typedef std::map<Symbol, AttrDef> AttrDefs;
    AttrDefs attrs;
    std::unique_ptr<std::vector<ref<Expr>>> inheritFromExprs;
    struct DynamicAttrDef {
        std::unique_ptr<Expr> nameExpr, valueExpr;
        PosIdx pos;
        DynamicAttrDef(std::unique_ptr<Expr> nameExpr, std::unique_ptr<Expr> valueExpr, const PosIdx & pos)
            : nameExpr(std::move(nameExpr)), valueExpr(std::move(valueExpr)), pos(pos) { };
    };
    typedef std::vector<DynamicAttrDef> DynamicAttrDefs;
    DynamicAttrDefs dynamicAttrs;
    ExprAttrs(const PosIdx &pos) : recursive(false), pos(pos) { };
    ExprAttrs() : recursive(false) { };
    PosIdx getPos() const override { return pos; }
    COMMON_METHODS

    std::shared_ptr<const StaticEnv> bindInheritSources(
        EvalState & es, const std::shared_ptr<const StaticEnv> & env);
    Env * buildInheritFromEnv(EvalState & state, Env & up);
    void showBindings(const SymbolTable & symbols, std::ostream & str) const;
};

struct ExprList : Expr
{
    std::vector<std::unique_ptr<Expr>> elems;
    ExprList() { };
    COMMON_METHODS
    Value * maybeThunk(EvalState & state, Env & env) override;

    PosIdx getPos() const override
    {
        return elems.empty() ? noPos : elems.front()->getPos();
    }
};

struct Formal
{
    PosIdx pos;
    Symbol name;
    std::unique_ptr<Expr> def;
};

/** Attribute set destructuring in arguments of a lambda, if present */
struct Formals
{
    typedef std::vector<Formal> Formals_;
    Formals_ formals;
    bool ellipsis;

    bool has(Symbol arg) const
    {
        auto it = std::lower_bound(formals.begin(), formals.end(), arg,
            [] (const Formal & f, const Symbol & sym) { return f.name < sym; });
        return it != formals.end() && it->name == arg;
    }

    std::vector<std::reference_wrapper<const Formal>> lexicographicOrder(const SymbolTable & symbols) const
    {
        std::vector<std::reference_wrapper<const Formal>> result(formals.begin(), formals.end());
        std::sort(result.begin(), result.end(),
            [&] (const Formal & a, const Formal & b) {
                std::string_view sa = symbols[a.name], sb = symbols[b.name];
                return sa < sb;
            });
        return result;
    }
};

struct ExprLambda : Expr
{
    /** Where the lambda is defined in Nix code. May be falsey if the
     * position is not known. */
    PosIdx pos;
    /** Name of the lambda. This is set if the lambda is defined in a
     * let-expression or an attribute set, such that there is a name.
     * Lambdas may have a falsey symbol as the name if they are anonymous */
    Symbol name;
    /** The argument name of this particular lambda. Is a falsey symbol if there
     * is no such argument. */
    Symbol arg;
    /** Formals are present when the lambda destructures an attr set as
     * argument, with or without ellipsis */
    std::unique_ptr<Formals> formals;
    std::unique_ptr<Expr> body;
    ExprLambda(PosIdx pos, Symbol arg, std::unique_ptr<Formals> formals, std::unique_ptr<Expr> body)
        : pos(pos), arg(arg), formals(std::move(formals)), body(std::move(body))
    {
    };
    ExprLambda(PosIdx pos, std::unique_ptr<Formals> formals, std::unique_ptr<Expr> body)
        : pos(pos), formals(std::move(formals)), body(std::move(body))
    {
    }
    void setName(Symbol name) override;
    std::string showNamePos(const EvalState & state) const;
    inline bool hasFormals() const { return formals != nullptr; }
    PosIdx getPos() const override { return pos; }

    /** Returns the name of the lambda,
     * or "anonymous lambda" if it doesn't have one.
     */
    inline std::string getName(SymbolTable const & symbols) const
    {
        if (this->name) {
            return symbols[this->name];
        }

        return "anonymous lambda";
    }

    /** Returns the name of the lambda in single quotes,
     * or "anonymous lambda" if it doesn't have one.
     */
    inline std::string getQuotedName(SymbolTable const & symbols) const
    {
        if (this->name) {
            return concatStrings("'", symbols[this->name], "'");
        }

        return "anonymous lambda";
    }

    COMMON_METHODS
};

struct ExprCall : Expr
{
    std::unique_ptr<Expr> fun;
    std::vector<std::unique_ptr<Expr>> args;
    PosIdx pos;
    ExprCall(const PosIdx & pos, std::unique_ptr<Expr> fun, std::vector<std::unique_ptr<Expr>> && args)
        : fun(std::move(fun)), args(std::move(args)), pos(pos)
    { }
    PosIdx getPos() const override { return pos; }
    COMMON_METHODS
};

struct ExprLet : Expr
{
    std::unique_ptr<ExprAttrs> attrs;
    std::unique_ptr<Expr> body;
    ExprLet(std::unique_ptr<ExprAttrs> attrs, std::unique_ptr<Expr> body) : attrs(std::move(attrs)), body(std::move(body)) { };
    COMMON_METHODS
};

struct ExprWith : Expr
{
    PosIdx pos;
    std::unique_ptr<Expr> attrs, body;
    size_t prevWith;
    ExprWith * parentWith;
    ExprWith(const PosIdx & pos, std::unique_ptr<Expr> attrs, std::unique_ptr<Expr> body) : pos(pos), attrs(std::move(attrs)), body(std::move(body)) { };
    PosIdx getPos() const override { return pos; }
    COMMON_METHODS
};

struct ExprIf : Expr
{
    PosIdx pos;
    std::unique_ptr<Expr> cond, then, else_;
    ExprIf(const PosIdx & pos, std::unique_ptr<Expr> cond, std::unique_ptr<Expr> then, std::unique_ptr<Expr> else_) : pos(pos), cond(std::move(cond)), then(std::move(then)), else_(std::move(else_)) { };
    PosIdx getPos() const override { return pos; }
    COMMON_METHODS
};

struct ExprAssert : Expr
{
    PosIdx pos;
    std::unique_ptr<Expr> cond, body;
    ExprAssert(const PosIdx & pos, std::unique_ptr<Expr> cond, std::unique_ptr<Expr> body) : pos(pos), cond(std::move(cond)), body(std::move(body)) { };
    PosIdx getPos() const override { return pos; }
    COMMON_METHODS
};

struct ExprOpNot : Expr
{
    std::unique_ptr<Expr> e;
    ExprOpNot(std::unique_ptr<Expr> e) : e(std::move(e)) { };
    PosIdx getPos() const override { return e->getPos(); }
    COMMON_METHODS
};

#define MakeBinOp(name, s) \
    struct name : Expr \
    { \
        PosIdx pos; \
        std::unique_ptr<Expr> e1, e2; \
        name(std::unique_ptr<Expr> e1, std::unique_ptr<Expr> e2) : e1(std::move(e1)), e2(std::move(e2)) { }; \
        name(const PosIdx & pos, std::unique_ptr<Expr> e1, std::unique_ptr<Expr> e2) : pos(pos), e1(std::move(e1)), e2(std::move(e2)) { }; \
        void show(const SymbolTable & symbols, std::ostream & str) const override \
        { \
            str << "("; e1->show(symbols, str); str << " " s " "; e2->show(symbols, str); str << ")"; \
        } \
        void bindVars(EvalState & es, const std::shared_ptr<const StaticEnv> & env) override \
        { \
            e1->bindVars(es, env); e2->bindVars(es, env);    \
        } \
        void eval(EvalState & state, Env & env, Value & v) override; \
        PosIdx getPos() const override { return pos; } \
    };

MakeBinOp(ExprOpEq, "==")
MakeBinOp(ExprOpNEq, "!=")
MakeBinOp(ExprOpAnd, "&&")
MakeBinOp(ExprOpOr, "||")
MakeBinOp(ExprOpImpl, "->")
MakeBinOp(ExprOpUpdate, "//")
MakeBinOp(ExprOpConcatLists, "++")

struct ExprConcatStrings : Expr
{
    PosIdx pos;
    bool forceString;
    std::vector<std::pair<PosIdx, std::unique_ptr<Expr>>> es;
    ExprConcatStrings(const PosIdx & pos, bool forceString, std::vector<std::pair<PosIdx, std::unique_ptr<Expr>>> es)
        : pos(pos), forceString(forceString), es(std::move(es)) { };
    PosIdx getPos() const override { return pos; }
    COMMON_METHODS
};

struct ExprPos : Expr
{
    PosIdx pos;
    ExprPos(const PosIdx & pos) : pos(pos) { };
    PosIdx getPos() const override { return pos; }
    COMMON_METHODS
};

/* only used to mark thunks as black holes. */
struct ExprBlackHole : Expr
{
    void show(const SymbolTable & symbols, std::ostream & str) const override {}
    void eval(EvalState & state, Env & env, Value & v) override;
    void bindVars(EvalState & es, const std::shared_ptr<const StaticEnv> & env) override {}
};

extern ExprBlackHole eBlackHole;


/* Static environments are used to map variable names onto (level,
   displacement) pairs used to obtain the value of the variable at
   runtime. */
struct StaticEnv
{
    ExprWith * isWith;
    const StaticEnv * up;

    // Note: these must be in sorted order.
    typedef std::vector<std::pair<Symbol, Displacement>> Vars;
    Vars vars;

    StaticEnv(ExprWith * isWith, const StaticEnv * up, size_t expectedSize = 0) : isWith(isWith), up(up) {
        vars.reserve(expectedSize);
    };

    void sort()
    {
        std::stable_sort(vars.begin(), vars.end(),
            [](const Vars::value_type & a, const Vars::value_type & b) { return a.first < b.first; });
    }

    void deduplicate()
    {
        auto it = vars.begin(), jt = it, end = vars.end();
        while (jt != end) {
            *it = *jt++;
            while (jt != end && it->first == jt->first) *it = *jt++;
            it++;
        }
        vars.erase(it, end);
    }

    Vars::const_iterator find(Symbol name) const
    {
        Vars::value_type key(name, 0);
        auto i = std::lower_bound(vars.begin(), vars.end(), key);
        if (i != vars.end() && i->first == name) return i;
        return vars.end();
    }
};


}
