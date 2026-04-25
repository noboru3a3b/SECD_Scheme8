
// ============================================================
// micro_scheme8.cpp
// SECD 仮想マシンベースの Micro Scheme インタプリタ / コンパイラ
//
// 構成概要:
//   1. オブジェクト定義        (Object, Type, DumpFrame, VMContext)
//   2. ヒープ・GC              (alloc, mark, sweep, gc)
//   3. ユーティリティ          (cons, make_int, get_symbol, print_*)
//   4. バイトコード命令定数     (OP_LD, OP_LDC, ...)
//   5. コンパイラ              (compile_subset_expr)
//   6. VM                     (vm)
//   7. トークナイザ・リーダ    (tokenize, s_read)
//   8. 評価器                 (eval_expr, eval_from_source)
//   9. プリミティブ登録        (register_core_primitives)
//  10. 初期化・REPL            (init_env, main)
// ============================================================
#include <iostream>
#include <vector>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <functional>
#include <memory>
#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <sstream>

// ------------------------------------------------------------
// SECD マシンのフレーム構造
// DumpFrame: 関数呼び出し時に保存するフレーム一式
//   s: スタックの中身, e: 環境, c: 続きのコード列, constants: 定数プール
// VMContext: 実行中の SECD レジスタ
//   s: スタック (S), e: 環境 (E), c: コード列 (C), d: ダンプ (D)
// ------------------------------------------------------------
struct Object;
struct DumpFrame {
    std::vector<Object*> s;               // 保存されたスタック
    std::vector<std::vector<Object*>> e;  // 保存された環境
    std::vector<int> c;                   // 保存されたコード列
    std::vector<Object*> constants;       // 保存された定数プール
};
struct VMContext {
    std::vector<Object*> s;               // S: スタック
    std::vector<std::vector<Object*>> e;  // E: 環境（フレームのリスト）
    std::vector<int> c;                   // C: バイトコード列（インデックスで参照）
    std::vector<DumpFrame> d;             // D: ダンプ（呼び出しスタック）
};

// ------------------------------------------------------------
// オブジェクト型の列挙
//   INT         : 32bit 整数
//   INT64       : 64bit 整数（オーバーフロー時に自動昇格）
//   DOUBLE      : 浮動小数点数（さらなるオーバーフロー時に昇格）
//   SYMBOL      : シンボル（識別子・キーワード）
//   PAIR        : cons セル（リストの基本構成要素）
//   CLOSURE     : クロージャ（コード + キャプチャされた環境）
//   CONTINUATION: 継続（call/cc で捕捉した実行コンテキスト）
//   PRIMITIVE   : C++ で実装した組み込み関数
//   PORT        : ファイルポート（入出力）
//   UNDEF       : 未定義値
// ------------------------------------------------------------
enum Type { INT, INT64, DOUBLE, SYMBOL, PAIR, CLOSURE, CONTINUATION, PRIMITIVE, PORT, UNDEF };

// ------------------------------------------------------------
// Object: ヒープ上のすべての Scheme 値を表す統一構造体
//   type   : 上記 Type 列挙値
//   marked : GC マーク（mark & sweep で使用）
//   union  : 型ごとのペイロード
// ------------------------------------------------------------
struct Object {
    Type type;
    bool marked = false;
    union {
        int num;
        long long num64;
        long double dbl;
        std::string* sym;
        struct { Object* car; Object* cdr; } pair;
        struct {
            std::vector<int>* code;
            std::vector<std::vector<Object*>>* env;
            std::vector<Object*>* constants;
        } closure;
        struct {
            std::fstream* file;
            bool is_output;
        } port;
        struct { 
            std::vector<Object*>* s; std::vector<std::vector<Object*>>* e;
            std::vector<int>* c; std::vector<DumpFrame>* d;
            std::vector<Object*>* constants;
        } continuation;
        std::function<Object*(std::vector<Object*>&)>* prim;
    };

    Object(Type t) : type(t), marked(false) {
        if (t == INT) num = 0;
        else if (t == INT64) num64 = 0;
        else if (t == DOUBLE) dbl = 0.0L;
        else if (t == SYMBOL) sym = nullptr;
        else if (t == PAIR) { pair.car = nullptr; pair.cdr = nullptr; }
        else if (t == CLOSURE) { closure.code = nullptr; closure.env = nullptr; closure.constants = nullptr; }
        else if (t == PORT) { port.file = nullptr; port.is_output = false; }
        else if (t == CONTINUATION) { continuation.s = nullptr; continuation.e = nullptr; 
                                     continuation.c = nullptr; continuation.d = nullptr; continuation.constants = nullptr; }
        else if (t == PRIMITIVE) prim = nullptr;
    }

    ~Object() {
        if (type == SYMBOL) delete sym;
        else if (type == CLOSURE) { delete closure.code; delete closure.env; delete closure.constants; }
        else if (type == PORT) {
            if (port.file) {
                if (port.file->is_open()) port.file->close();
                delete port.file;
            }
        }
        else if (type == CONTINUATION) { 
            delete continuation.s; delete continuation.e; 
            delete continuation.c; delete continuation.d; delete continuation.constants;
        }
        else if (type == PRIMITIVE) delete prim;
    }
};

// ------------------------------------------------------------
// グローバル状態
//   heap             : GC 管理対象のすべてのオブジェクトポインタ
//   globals          : 大域変数テーブル（シンボル名 → Object*）
//   macros           : マクロテーブル（シンボル名 → クロージャ）
//   symbols          : シンボルインターン表（同名は同一オブジェクトを共有）
//   int_cache        : 小整数キャッシュ（再確保を避ける最適化）
//   g_eval_env_stack : ツリーウォーク評価器のレキシカル環境スタック
//   g_closure_lexenv : eval 系クロージャのキャプチャ環境
//   MAX_OBJECTS      : GC を起動するヒープサイズの閾値
//   SMALL_INT_MIN/MAX: キャッシュ対象の整数範囲
//   g_active_ctx     : 現在実行中の VMContext（alloc から GC を呼ぶため）
//   g_active_constants: 現在実行中の定数プール（GC のルート走査に使用）
// ------------------------------------------------------------
std::vector<Object*> heap;
std::unordered_map<std::string, Object*> globals;
std::unordered_map<std::string, Object*> macros;
std::unordered_map<std::string, Object*> symbols;
std::unordered_map<int, Object*> int_cache;
using EvalFrame = std::unordered_map<std::string, Object*>;
using EvalEnv = std::vector<EvalFrame>;
std::vector<EvalFrame> g_eval_env_stack;
std::unordered_map<Object*, EvalEnv> g_closure_lexenv;
int g_vm_subset_success_count = 0;
const size_t MAX_OBJECTS = 10000;   // この件数を超えたら GC を発動
const int SMALL_INT_MIN = -1024;   // 整数キャッシュの下限
const int SMALL_INT_MAX = 4096;    // 整数キャッシュの上限

VMContext* g_active_ctx = nullptr;             // alloc 時に参照する実行コンテキスト
std::vector<Object*>* g_active_constants = nullptr; // alloc 時に参照する定数プール

Object* vm(VMContext& ctx, std::vector<Object*>& constants);
Object* eval_expr(Object* expr);
Object* eval_from_source(const std::string& source, VMContext& ctx, std::vector<Object*>& constants);
std::vector<Object*> read_all_exprs_from_string(const std::string& text);
bool load_scheme_file(const std::string& path);
Object* prim_read_impl(std::vector<Object*>& args);

struct ParseError : public std::runtime_error {
    explicit ParseError(const std::string& msg) : std::runtime_error(msg) {}
};
struct VMError : public std::runtime_error {
    explicit VMError(const std::string& msg) : std::runtime_error(msg) {}
};

struct ContinuationEscape {
    Object* tag;
    Object* value;
};

struct ParamSpec {
    std::vector<std::string> fixed;
    bool has_rest = false;
    std::string rest;
};

// --- 追加する前方宣言 ---
Object* vector_to_pair(const std::vector<Object*>& items);
bool is_symbol_name(Object* obj, const std::string& name);
Object* backquote_transfer(Object* expr);
bool parse_binding_list(Object* bindings_expr, std::vector<std::pair<std::string, Object*>>& out);
bool is_tagged_form(Object* obj, const std::string& tag);
// ------------------------

bool extract_param_spec(Object* params_expr, ParamSpec& out) {
    out = ParamSpec{};
    std::unordered_set<std::string> seen;

    if (!params_expr) {
        return true;
    }

    if (params_expr->type == SYMBOL && params_expr->sym) {
        out.has_rest = true;
        out.rest = *params_expr->sym;
        return true;
    }

    Object* cur = params_expr;
    while (cur && cur->type == PAIR) {
        Object* p = cur->pair.car;
        if (!p || p->type != SYMBOL || !p->sym) {
            return false;
        }
        const std::string& name = *p->sym;
        if (seen.count(name) > 0) return false;
        seen.insert(name);
        out.fixed.push_back(name);
        cur = cur->pair.cdr;
    }

    if (cur) {
        if (cur->type != SYMBOL || !cur->sym) return false;
        if (seen.count(*cur->sym) > 0) return false;
        out.has_rest = true;
        out.rest = *cur->sym;
    }

    return true;
}

bool extract_param_names(Object* params_expr, std::vector<std::string>& out) {
    ParamSpec spec;
    if (!extract_param_spec(params_expr, spec) || spec.has_rest) {
        return false;
    }
    out = spec.fixed;
    return true;
}

bool bind_params_to_globals(const ParamSpec& spec, const std::vector<Object*>& args,
                            std::unordered_map<std::string, Object*>& saved,
                            std::vector<std::string>& new_bindings) {
    if (!spec.has_rest && spec.fixed.size() != args.size()) return false;
    if (spec.has_rest && args.size() < spec.fixed.size()) return false;

    auto bind_one = [&](const std::string& name, Object* value) {
        auto it = globals.find(name);
        if (it != globals.end()) {
            saved[name] = it->second;
        } else {
            new_bindings.push_back(name);
        }
        globals[name] = value;
    };

    for (size_t i = 0; i < spec.fixed.size(); ++i) {
        bind_one(spec.fixed[i], args[i]);
    }

    if (spec.has_rest) {
        std::vector<Object*> rest_items(args.begin() + static_cast<long long>(spec.fixed.size()), args.end());
        bind_one(spec.rest, vector_to_pair(rest_items));
    }

    return true;
}

Object* bool_obj(bool v) {
    return v ? globals["true"] : globals["false"];
}

bool is_true(Object* v) {
    return v != globals["false"];
}

bool objects_equal(Object* a, Object* b) {
    if (a == b) return true;
    if (!a || !b) return false;
    if (a->type != b->type) return false;
    if (a->type == INT) return a->num == b->num;
    if (a->type == SYMBOL && a->sym && b->sym) return *a->sym == *b->sym;
    if (a->type == PAIR) {
        return objects_equal(a->pair.car, b->pair.car) && objects_equal(a->pair.cdr, b->pair.cdr);
    }
    return false;
}


// ------------------------------------------------------------
// GC: マーク & スイープ方式
//
// mark(obj)  : obj を起点に到達可能な全オブジェクトに marked フラグを立てる
// sweep()    : marked でないオブジェクトを delete して heap から除去する
// gc(ctx, constants, silent)
//            : ルート集合（スタック・環境・グローバル変数等）から
//              mark を呼び出し、sweep で不要オブジェクトを回収する
//              silent=true のときは [GC] メッセージを出力しない
// alloc(t)   : heap が MAX_OBJECTS に達したら gc を発動してから確保する
// ------------------------------------------------------------
void mark(Object* obj) {
    if (!obj || obj->marked) return;
    obj->marked = true;

    if (obj->type == PAIR) {
        mark(obj->pair.car);
        mark(obj->pair.cdr);
    } else if (obj->type == CLOSURE) {
        if (obj->closure.env) {
            for (auto& frame : *obj->closure.env) {
                for (auto& item : frame) mark(item);
            }
        }
        if (obj->closure.constants) {
            for (auto& item : *obj->closure.constants) mark(item);
        }
    } else if (obj->type == CONTINUATION) {
        if (obj->continuation.s) for (auto& item : *obj->continuation.s) mark(item);
        if (obj->continuation.e) {
            for (auto& frame : *obj->continuation.e) {
                for (auto& item : frame) mark(item);
            }
        }
        if (obj->continuation.d) {
            for (auto& frame : *obj->continuation.d) {
                for (auto& s_item : frame.s) mark(s_item);
                for (auto& e_frame : frame.e) {
                    for (auto& e_item : e_frame) mark(e_item);
                }
            }
        }
        if (obj->continuation.constants) {
            for (auto& item : *obj->continuation.constants) mark(item);
        }
    }
}

void sweep(bool silent = false) {
    auto it = std::remove_if(heap.begin(), heap.end(), [](Object* obj) {
        if (obj->marked) {
            obj->marked = false;
            return false;
        }
        delete obj;
        return true;
    });
    
    size_t collected = std::distance(it, heap.end());
    if (!silent && collected > 0) {
        std::cout << "[GC] Collected " << collected << " objects." << std::endl;
    }
    heap.erase(it, heap.end());
}

void gc(VMContext& ctx, std::vector<Object*>& constants, bool silent = false) {
    for (auto& obj : ctx.s) mark(obj);
    for (auto& frame : ctx.e) {
        for (auto& obj : frame) mark(obj);
    }
    for (auto& obj : constants) mark(obj);
    for (auto& pair : globals) mark(pair.second);
    for (auto& pair : macros) mark(pair.second);
    for (auto& pair : symbols) mark(pair.second);
    for (auto& pair : int_cache) mark(pair.second);
    for (auto& frame : g_eval_env_stack) {
        for (auto& kv : frame) mark(kv.second);
    }
    for (auto& closure_env : g_closure_lexenv) {
        for (auto& frame : closure_env.second) {
            for (auto& kv : frame) mark(kv.second);
        }
    }
    for (auto& frame : ctx.d) {
        for (auto& s_item : frame.s) mark(s_item);
        for (auto& e_frame : frame.e) {
            for (auto& e_item : e_frame) mark(e_item);
        }
    }

    sweep(silent);
}

Object* alloc(Type t) {
    if (g_active_ctx && g_active_constants && heap.size() >= MAX_OBJECTS) {
        gc(*g_active_ctx, *g_active_constants);
    }
    Object* obj = new Object(t);
    heap.push_back(obj);
    return obj;
}

// ------------------------------------------------------------
// オーバーフロー検出関数
//   32bit 整数の演算でオーバーフローが発生するかをチェック
// ------------------------------------------------------------
bool will_overflow_add(int a, int b) {
    return (b > 0 && a > INT_MAX - b) || (b < 0 && a < INT_MIN - b);
}

bool will_overflow_sub(int a, int b) {
    return (b < 0 && a > INT_MAX + b) || (b > 0 && a < INT_MIN + b);
}

bool will_overflow_mul(int a, int b) {
    if (a == 0 || b == 0) return false;
    long long res = (long long)a * b;
    return res > INT_MAX || res < INT_MIN;
}

// Forward declaration: make_int, make_number, make_number_from_double
Object* make_int(int val);
Object* make_number(long long val);
Object* make_number_from_double(long double val);

// ------------------------------------------------------------
// ユーティリティ: オブジェクト生成
//   cons(car, cdr)  : PAIR オブジェクトを生成
//   make_int(val)   : INT オブジェクトを生成（小整数はキャッシュから返す）
//   get_symbol(str) : シンボルをインターンして返す（"nil" は nullptr）
//   make_symbol_obj : インターンしない独立したシンボルを生成（主に read 用）
// ------------------------------------------------------------
Object* cons(Object* car, Object* cdr) {
    Object* obj = alloc(PAIR);
    obj->pair.car = car;
    obj->pair.cdr = cdr;
    return obj;
}

Object* make_int(int val) {
    if (val >= SMALL_INT_MIN && val <= SMALL_INT_MAX) {
        auto it = int_cache.find(val);
        if (it != int_cache.end()) return it->second;
        Object* obj = alloc(INT);
        obj->num = val;
        int_cache[val] = obj;
        return obj;
    }
    Object* obj = alloc(INT);
    obj->num = val;
    return obj;
}

// 値に応じて適切な型に昇格させたオブジェクトを返す
// 32bit に収まれば INT、64bit に収まれば INT64、それ以上は DOUBLE
Object* make_number(long long val) {
    if (val >= INT_MIN && val <= INT_MAX) {
        return make_int((int)val);
    }
    Object* obj = alloc(INT64);
    obj->num64 = val;
    return obj;
}

Object* make_number_from_double(long double val) {
    // 有限値か確認
    if (!std::isfinite(val)) {
        Object* obj = alloc(DOUBLE);
        obj->dbl = val;
        return obj;
    }
    
    // 小数部を抽出（精度を考慮）
    long double frac_part = val - floorl(val);
    if (frac_part > 0.5L) frac_part = 1.0L - frac_part;  // 上への丸めの場合も考慮
    
    // 小数部がほぼ0ならば整数として扱う
    const long double epsilon = 1e-15L;
    if (fabsl(frac_part) < epsilon || (1.0L - fabsl(frac_part)) < epsilon) {
        // llroundl は範囲外値に対して未定義/実装依存の結果になり得るため、
        // 先に long long の範囲判定を行う。
        if (val >= (long double)LLONG_MIN && val <= (long double)LLONG_MAX) {
            long long as_ll = llroundl(val);  // 四捨五入で整数に変換

            // INT に収まるか試す
            if (as_ll >= INT_MIN && as_ll <= INT_MAX) {
                return make_int((int)as_ll);
            }
            // INT64 に収まるか試す
            return make_number(as_ll);
        }
    }
    
    // DOUBLE として保存
    Object* obj = alloc(DOUBLE);
    obj->dbl = val;
    return obj;
}

Object* get_symbol(const std::string& str) {
    if (str == "nil") return nullptr;
    if (symbols.find(str) == symbols.end()) {
        Object* obj = alloc(SYMBOL);
        obj->sym = new std::string(str);
        symbols[str] = obj;
    }
    return symbols[str];
}

Object* make_symbol_obj(const std::string& str) {
    Object* obj = alloc(SYMBOL);
    obj->sym = new std::string(str);
    return obj;
}

void print_obj(Object* obj);
void print_obj_stream(Object* obj, std::ostream& os);
void print_code_stream(const std::vector<int>& code, std::ostream& os, const std::vector<Object*>* constants = nullptr);

void print_list_inner_stream(Object* obj, std::ostream& os) {
    if (!obj) return;
    if (obj->type != PAIR) {
        os << ". ";
        print_obj_stream(obj, os);
        return;
    }
    print_obj_stream(obj->pair.car, os);
    if (obj->pair.cdr) {
        os << " ";
        print_list_inner_stream(obj->pair.cdr, os);
    }
}

void print_obj_stream(Object* obj, std::ostream& os) {
    if (!obj) { os << "nil"; return; }
    switch (obj->type) {
        case INT: os << obj->num; break;
        case INT64: os << obj->num64; break;
        case DOUBLE: os << obj->dbl; break;
        case SYMBOL: os << *obj->sym; break;
        case PAIR:
            os << "(";
            print_list_inner_stream(obj, os);
            os << ")";
            break;
        case CLOSURE: {
            os << "(CLOSURE ";
            if (obj->closure.code) {
                print_code_stream(*obj->closure.code, os, obj->closure.constants);
            } else {
                os << "()";
            }
            os << " ";
            if (obj->closure.env && !obj->closure.env->empty()) {
                os << "(";
                bool first = true;
                for (const auto& frame : *obj->closure.env) {
                    if (!first) os << " ";
                    os << "(";
                    bool first_var = true;
                    for (const auto& var : frame) {
                        if (!first_var) os << " ";
                        print_obj_stream(var, os);
                        first_var = false;
                    }
                    os << ")";
                    first = false;
                }
                os << ")";
            } else {
                os << "NIL";
            }
            os << ")";
            break;
        }
        case PRIMITIVE:
            os << "#<primitive>";
            break;
        case CONTINUATION:
            os << "#<continuation>";
            break;
        case UNDEF:
            os << "#<undef>";
            break;
        case PORT:
            os << (obj->port.is_output ? "<port:output>" : "<port:input>");
            break;
        default: os << "<obj>"; break;
    }
}

void print_obj(Object* obj) {
    print_obj_stream(obj, std::cout);
}

std::string object_to_string(Object* obj) {
    std::ostringstream oss;
    print_obj_stream(obj, oss);
    return oss.str();
}

// ------------------------------------------------------------
// バイトコード命令定数
//
// ロード系:
//   OP_LD    (0)  : LD (i j)  - 環境フレーム i の j 番目の変数をスタックへ
//   OP_LDC   (1)  : LDC idx   - 定数プール[idx] をスタックへ
//   OP_LDG   (2)  : LDG idx   - 定数プール[idx] のシンボル名の大域変数をスタックへ
//   OP_LDF   (3)  : LDF idx   - 定数プール[idx] のコードからクロージャを生成してスタックへ
//   OP_LDCT  (4)  : LDCT      - call/cc 用継続を生成してスタックへ
//   OP_LDNIL (20) : LDNIL     - nil をスタックへ
//   OP_LDTRUE(21) : LDTRUE    - true をスタックへ
//   OP_LDFALSE(22): LDFALSE   - false をスタックへ
//
// ストア系:
//   OP_LSET  (5)  : LSET (i j) - スタックトップを環境フレーム i の j 番目へ格納
//   OP_GSET  (6)  : GSET idx   - スタックトップを大域変数へ格納
//   OP_DEF  (13)  : DEF idx    - スタックトップを大域変数に定義（define）
//   OP_DEFM (14)  : DEFM idx   - スタックトップをマクロとして定義（define-macro）
//
// 呼び出し系:
//   OP_CALL  (16) : CALL n    - ダンプに継続を保存して n 引数でクロージャを呼び出す
//   OP_TCALL (17) : TCALL n   - 末尾呼び出し。ダンプを保存せず既存フレームを再利用
//   OP_CALLG (18) : CALLG idx n - 大域変数の関数を n 引数で呼び出す
//   OP_TCALLG(19) : TCALLG idx n - 大域変数の末尾呼び出し
//   OP_APP   (7)  : APP n     - 引数リスト版呼び出し（apply 系）
//   OP_TAPP  (8)  : TAPP n   - 引数リスト版末尾呼び出し
//   OP_CALLCC(33) : CALLCC   - call/cc：現在の継続を捕捉して関数を呼ぶ
//
// リターン・ジャンプ系:
//   OP_RTN  (9)   : RTN       - ダンプからレジスタを復元して呼び出し元へ戻る
//   OP_STOP (15)  : STOP      - VM 実行を停止
//   OP_JZ   (31)  : JZ offset - スタックトップが false なら offset だけ PC を進める
//   OP_JMP  (32)  : JMP offset- 無条件ジャンプ
//   OP_POP  (10)  : POP       - スタックトップを破棄
//
// 引数リスト構築:
//   OP_ARGS    (11): ARGS n    - スタックから n 個を取り出してリストを作りスタックへ
//   OP_ARGS_AP (12): ARGS-AP n - apply 用引数リスト構築
// ------------------------------------------------------------
constexpr int OP_LD = 0;
constexpr int OP_LDC = 1;
constexpr int OP_LDG = 2;
constexpr int OP_LDF = 3;
constexpr int OP_LDCT = 4;
constexpr int OP_LSET = 5;
constexpr int OP_GSET = 6;
constexpr int OP_APP = 7;
constexpr int OP_TAPP = 8;
constexpr int OP_ARGS = 11;
constexpr int OP_ARGS_AP = 12;
constexpr int OP_DEF = 13;
constexpr int OP_DEFM = 14;
constexpr int OP_STOP = 15;
constexpr int OP_RTN = 9;
constexpr int OP_CALL = 16;
constexpr int OP_TCALL = 17;
constexpr int OP_CALLG = 18;
constexpr int OP_TCALLG = 19;
constexpr int OP_LDNIL = 20;
constexpr int OP_LDTRUE = 21;
constexpr int OP_LDFALSE = 22;
constexpr int OP_JZ = 31;
constexpr int OP_JMP = 32;
constexpr int OP_CALLCC = 33;
constexpr int OP_POP = 10;

std::string op_to_string(int op) {
    switch (op) {
        case OP_LD: return "LD";
        case OP_LDC: return "LDC";
        case OP_LDG: return "LDG";
        case OP_LDF: return "LDF";
        case OP_LDCT: return "LDCT";
        case OP_LSET: return "LSET";
        case OP_GSET: return "GSET";
        case OP_APP: return "APP";
        case OP_TAPP: return "TAPP";
        case OP_ARGS: return "ARGS";
        case OP_ARGS_AP: return "ARGS-AP";
        case OP_DEF: return "DEF";
        case OP_DEFM: return "DEFM";
        case OP_STOP: return "STOP";
        case OP_RTN: return "RTN";
        case OP_CALL: return "CALL";
        case OP_TCALL: return "TCALL";
        case OP_CALLG: return "CALLG";
        case OP_TCALLG: return "TCALLG";
        case OP_LDNIL: return "LDNIL";
        case OP_LDTRUE: return "LDTRUE";
        case OP_LDFALSE: return "LDFALSE";
        case OP_JZ: return "JZ";
        case OP_JMP: return "JMP";
        case OP_CALLCC: return "CALLCC";
        case OP_POP: return "POP";
        default: return "UNKNOWN";
    }
}

void print_code_stream(const std::vector<int>& code, std::ostream& os, const std::vector<Object*>* constants) {
    if (code.empty()) { os << "()"; return; }

    auto print_const = [&](int idx) {
        if (constants && idx >= 0 && static_cast<size_t>(idx) < constants->size()) {
            print_obj_stream((*constants)[static_cast<size_t>(idx)], os);
        } else {
            os << idx;
        }
    };

    os << "(";
    for (size_t i = 0; i < code.size(); ) {
        int op = code[i];
        if (i > 0) os << " ";
        os << op_to_string(op);
        i++;
        
        if (op == OP_LD && i + 1 < code.size()) {
            int d = code[i];
            int v = code[i+1];
            os << " (" << d << " . " << v << ")";
            i += 2;
        } else if (op == OP_LDC && i < code.size()) {
            int idx = code[i++];
            os << " ";
            print_const(idx);
        } else if (op == OP_LDG && i < code.size()) {
            int idx = code[i++];
            os << " ";
            print_const(idx);
        } else if (op == OP_LDF && i < code.size()) {
            int idx = code[i++];
            os << " ";
            if (constants && idx >= 0 && static_cast<size_t>(idx) < constants->size()) {
                Object* nested = (*constants)[static_cast<size_t>(idx)];
                if (nested && nested->type == CLOSURE && nested->closure.code) {
                    print_code_stream(*nested->closure.code, os, constants);
                } else {
                    print_const(idx);
                }
            } else {
                os << idx;
            }
        } else if (op == OP_LSET && i + 1 < code.size()) {
            int d = code[i];
            int v = code[i+1];
            os << " (" << d << " . " << v << ")";
            i += 2;
        } else if ((op == OP_GSET || op == OP_DEF || op == OP_DEFM) && i < code.size()) {
            int idx = code[i++];
            os << " ";
            print_const(idx);
        } else if ((op == OP_APP || op == OP_TAPP || op == OP_CALL || op == OP_TCALL || op == OP_ARGS || op == OP_ARGS_AP) && i < code.size()) {
            int num_args = code[i++];
            os << " " << num_args;
        } else if ((op == OP_CALLG || op == OP_TCALLG) && i + 1 < code.size()) {
            int idx = code[i];
            int num_args = code[i+1];
            os << " ";
            print_const(idx);
            os << " " << num_args;
            i += 2;
        } else if (op == OP_JZ || op == OP_JMP) {
            if (i < code.size()) {
                int addr = code[i];
                os << " " << addr;
                i++;
            }
        }
    }
    os << ")";
}

Object* get_lvar(std::vector<std::vector<Object*>>& e, size_t i, size_t j) {
    if (i < e.size() && j < e[i].size()) return e[i][j];
    return nullptr;
}

void set_lvar(std::vector<std::vector<Object*>>& e, size_t i, size_t j, Object* val) {
    if (i < e.size() && j < e[i].size()) {
        e[i][j] = val;
    } else {
        std::cerr << "VM Error: LVAR access out of range (" << i << ", " << j << ")" << std::endl;
    }
}

std::vector<Object*> pair_to_vector(Object* lst) {
    std::vector<Object*> out;
    Object* cur = lst;
    while (cur) {
        if (cur->type != PAIR) {
            break;
        }
        out.push_back(cur->pair.car);
        cur = cur->pair.cdr;
    }
    return out;
}

Object* vector_to_pair(const std::vector<Object*>& items) {
    Object* out = nullptr;
    for (auto it = items.rbegin(); it != items.rend(); ++it) {
        out = cons(*it, out);
    }
    return out;
}

std::vector<int> resolve_clause_operand(int operand, const std::vector<Object*>& constants) {
    size_t idx = static_cast<size_t>(operand);
    if (idx < constants.size() && constants[idx] && constants[idx]->type == CLOSURE && constants[idx]->closure.code) {
        return *constants[idx]->closure.code;
    }
    return {};
}

int add_constant(std::vector<Object*>& constants, Object* obj) {
    constants.push_back(obj);
    return static_cast<int>(constants.size() - 1);
}

using CompileEnv = std::vector<std::vector<std::string>>;

bool find_local(const CompileEnv& env, const std::string& name, int& level, int& index) {
    for (size_t i = 0; i < env.size(); ++i) {
        const auto& frame = env[i];
        for (size_t j = 0; j < frame.size(); ++j) {
            if (frame[j] == name) {
                level = static_cast<int>(i);
                index = static_cast<int>(j);
                return true;
            }
        }
    }
    return false;
}

Object* make_vm_code_closure_constant(const std::vector<int>& body_code) {
    Object* clo = alloc(CLOSURE);
    clo->closure.code = new std::vector<int>(body_code);
    clo->closure.env = new std::vector<std::vector<Object*>>();
    clo->closure.constants = new std::vector<Object*>();
    return clo;
}

// ------------------------------------------------------------
// コンパイラ: compile_subset_expr
//
// Scheme 式 expr を SECD バイトコードに変換する。
//
// 引数:
//   expr      : コンパイル対象の Scheme 式
//   code      : 出力先バイトコード列（末尾に追記される）
//   constants : 出力先定数プール（add_constant で追記）
//   env       : コンパイル時のレキシカル環境（変数名 → (depth, index) 解決に使用）
//   tail      : 末尾位置フラグ。true のとき CALL → TCALL、CALLG → TCALLG を発行する
//
// 末尾呼び出し最適化 (TCO):
//   tail=true のとき、関数呼び出しは TCALL/TCALLG を発行する。
//   TCALL/TCALLG はダンプにフレームを積まないため、深い再帰でも
//   dump スタックが増加せず、定数空間で実行できる。
//
// 対応する構文:
//   自己評価（整数・クロージャ等）, シンボル参照, quote, backquote,
//   if, begin, lambda, define, define-macro, set!,
//   let, let*, letrec, and, or, cond, when, unless, call/cc,
//   一般関数呼び出し
// ------------------------------------------------------------
bool compile_subset_expr(Object* expr, std::vector<int>& code, std::vector<Object*>& constants, const CompileEnv& env, bool tail = false) {
    if (!expr) {
        code.push_back(OP_LDNIL);
        return true;
    }

    if (expr->type == INT || expr->type == PRIMITIVE || expr->type == CLOSURE || expr->type == CONTINUATION) {
        code.push_back(OP_LDC);
        code.push_back(add_constant(constants, expr));
        return true;
    }

    if (expr->type == SYMBOL) {
        if (!expr->sym) return false;
        int level = 0;
        int index = 0;
        if (find_local(env, *expr->sym, level, index)) {
            code.push_back(OP_LD);
            code.push_back(level);
            code.push_back(index);
            return true;
        }
        auto git = globals.find(*expr->sym);
        if (git == globals.end() && env.empty()) return false;
        code.push_back(OP_LDG);
        code.push_back(add_constant(constants, expr));
        return true;
    }

    if (expr->type != PAIR) {
        return false;
    }

    Object* head = expr->pair.car;
    Object* rest = expr->pair.cdr;

    if (is_symbol_name(head, "quote")) {
        Object* quoted = (rest && rest->type == PAIR) ? rest->pair.car : nullptr;
        code.push_back(OP_LDC);
        code.push_back(add_constant(constants, quoted));
        return true;
    }

    if (is_symbol_name(head, "backquote")) {
        Object* arg = (rest && rest->type == PAIR) ? rest->pair.car : nullptr;
        return compile_subset_expr(backquote_transfer(arg), code, constants, env);
    }

    if (is_symbol_name(head, "call/cc")) {
        Object* fn_expr = (rest && rest->type == PAIR) ? rest->pair.car : nullptr;
        if (!compile_subset_expr(fn_expr, code, constants, env)) return false;
        code.push_back(OP_CALLCC);
        return true;
    }

    if (is_symbol_name(head, "if")) {
        Object* test_expr = (rest && rest->type == PAIR) ? rest->pair.car : nullptr;
        Object* tail1 = (rest && rest->type == PAIR) ? rest->pair.cdr : nullptr;
        Object* then_expr = (tail1 && tail1->type == PAIR) ? tail1->pair.car : nullptr;
        Object* tail2 = (tail1 && tail1->type == PAIR) ? tail1->pair.cdr : nullptr;
        Object* else_expr = (tail2 && tail2->type == PAIR) ? tail2->pair.car : nullptr;

        if (!compile_subset_expr(test_expr, code, constants, env)) return false;

        code.push_back(OP_JZ);
        int jz_operand_index = static_cast<int>(code.size());
        code.push_back(0);

        if (!compile_subset_expr(then_expr, code, constants, env, tail)) return false;

        code.push_back(OP_JMP);
        int jmp_operand_index = static_cast<int>(code.size());
        code.push_back(0);

        int else_start = static_cast<int>(code.size());
        if (!compile_subset_expr(else_expr, code, constants, env, tail)) return false;
        int end_pos = static_cast<int>(code.size());

        code[jz_operand_index] = else_start - (jz_operand_index + 1);
        code[jmp_operand_index] = end_pos - (jmp_operand_index + 1);
        return true;
    }

    if (is_symbol_name(head, "begin")) {
        std::vector<Object*> forms = pair_to_vector(rest);
        if (forms.empty()) {
            code.push_back(OP_LDNIL);
            return true;
        }
        for (size_t i = 0; i < forms.size(); ++i) {
            bool is_last = (i + 1 == forms.size());
            if (!compile_subset_expr(forms[i], code, constants, env, tail && is_last)) return false;
            if (!is_last) {
                code.push_back(OP_POP);
            }
        }
        return true;
    }

    if (is_symbol_name(head, "lambda")) {
        Object* params_expr = (rest && rest->type == PAIR) ? rest->pair.car : nullptr;
        Object* body_list = (rest && rest->type == PAIR) ? rest->pair.cdr : nullptr;
        ParamSpec spec;
        if (!extract_param_spec(params_expr, spec) || spec.has_rest) return false;

        CompileEnv body_env = env;
        body_env.insert(body_env.begin(), spec.fixed);

        std::vector<Object*> body_forms = pair_to_vector(body_list);
        std::vector<int> body_code;
        if (body_forms.empty()) {
            body_code.push_back(OP_LDNIL);
        } else {
            for (size_t i = 0; i < body_forms.size(); ++i) {
                bool is_last = (i + 1 == body_forms.size());
                if (!compile_subset_expr(body_forms[i], body_code, constants, body_env, is_last)) return false;
                if (!is_last) body_code.push_back(OP_POP);
            }
        }
        body_code.push_back(OP_RTN);

        Object* clo_const = make_vm_code_closure_constant(body_code);
        code.push_back(OP_LDF);
        code.push_back(add_constant(constants, clo_const));
        return true;
    }

    if (is_symbol_name(head, "define")) {
        Object* name_obj = (rest && rest->type == PAIR) ? rest->pair.car : nullptr;
        Object* tail = (rest && rest->type == PAIR) ? rest->pair.cdr : nullptr;
        if (!name_obj) return false;

        if (name_obj->type == SYMBOL && name_obj->sym) {
            Object* val_expr = (tail && tail->type == PAIR) ? tail->pair.car : nullptr;
            if (!compile_subset_expr(val_expr, code, constants, env)) return false;
            code.push_back(OP_DEF);
            code.push_back(add_constant(constants, name_obj));
            return true;
        }

        if (name_obj->type == PAIR && name_obj->pair.car && name_obj->pair.car->type == SYMBOL && name_obj->pair.car->sym) {
            Object* fname = name_obj->pair.car;
            Object* params_expr = name_obj->pair.cdr;
            Object* body_list = tail;
            Object* lambda_expr = cons(get_symbol("lambda"), cons(params_expr, body_list));
            if (!compile_subset_expr(lambda_expr, code, constants, env)) return false;
            code.push_back(OP_DEF);
            code.push_back(add_constant(constants, fname));
            return true;
        }
        return false;
    }

    if (is_symbol_name(head, "define-macro")) {
        Object* name_obj = (rest && rest->type == PAIR) ? rest->pair.car : nullptr;
        Object* tail = (rest && rest->type == PAIR) ? rest->pair.cdr : nullptr;
        if (!name_obj) return false;

        if (name_obj->type == SYMBOL && name_obj->sym) {
            Object* val_expr = (tail && tail->type == PAIR) ? tail->pair.car : nullptr;
            if (!compile_subset_expr(val_expr, code, constants, env)) return false;
            code.push_back(OP_DEFM);
            code.push_back(add_constant(constants, name_obj));
            return true;
        }

        if (name_obj->type == PAIR && name_obj->pair.car && name_obj->pair.car->type == SYMBOL && name_obj->pair.car->sym) {
            Object* mname = name_obj->pair.car;
            Object* params_expr = name_obj->pair.cdr;
            Object* body_list = tail;
            Object* lambda_expr = cons(get_symbol("lambda"), cons(params_expr, body_list));
            if (!compile_subset_expr(lambda_expr, code, constants, env)) return false;
            code.push_back(OP_DEFM);
            code.push_back(add_constant(constants, mname));
            return true;
        }
        return false;
    }

    if (is_symbol_name(head, "set!")) {
        Object* name_obj = (rest && rest->type == PAIR) ? rest->pair.car : nullptr;
        Object* tail = (rest && rest->type == PAIR) ? rest->pair.cdr : nullptr;
        Object* val_expr = (tail && tail->type == PAIR) ? tail->pair.car : nullptr;
        if (!name_obj || name_obj->type != SYMBOL || !name_obj->sym) return false;
        if (!compile_subset_expr(val_expr, code, constants, env)) return false;
        int level = 0;
        int index = 0;
        if (find_local(env, *name_obj->sym, level, index)) {
            code.push_back(OP_LSET);
            code.push_back(level);
            code.push_back(index);
        } else {
            code.push_back(OP_GSET);
            code.push_back(add_constant(constants, name_obj));
        }
        return true;
    }

    if (is_symbol_name(head, "let") || is_symbol_name(head, "let*") || is_symbol_name(head, "letrec")) {
        Object* bindings_expr = (rest && rest->type == PAIR) ? rest->pair.car : nullptr;
        Object* body_list = (rest && rest->type == PAIR) ? rest->pair.cdr : nullptr;
        std::vector<std::pair<std::string, Object*>> binds;
        if (!parse_binding_list(bindings_expr, binds)) return false;

        auto make_binding = [&](const std::string& name, Object* val_expr) {
            return cons(get_symbol(name), cons(val_expr, nullptr));
        };

        if (is_symbol_name(head, "let")) {
            std::vector<std::string> params;
            for (auto& b : binds) {
                if (!compile_subset_expr(b.second, code, constants, env)) return false;
                params.push_back(b.first);
            }

            CompileEnv body_env = env;
            body_env.insert(body_env.begin(), params);
            std::vector<Object*> forms = pair_to_vector(body_list);
            std::vector<int> body_code;
            if (forms.empty()) body_code.push_back(OP_LDNIL);
            else {
                for (size_t i = 0; i < forms.size(); ++i) {
                    bool is_last = (i + 1 == forms.size());
                    if (!compile_subset_expr(forms[i], body_code, constants, body_env, is_last)) return false;
                    if (!is_last) body_code.push_back(OP_POP);
                }
            }
            body_code.push_back(OP_RTN);
            Object* clo_const = make_vm_code_closure_constant(body_code);
            code.push_back(OP_LDF);
            code.push_back(add_constant(constants, clo_const));
            code.push_back(OP_CALL);
            code.push_back(static_cast<int>(binds.size()));
            return true;
        }

        if (is_symbol_name(head, "let*")) {
            if (binds.empty()) {
                std::vector<Object*> forms = pair_to_vector(body_list);
                if (forms.empty()) {
                    code.push_back(OP_LDNIL);
                    return true;
                }
                for (size_t i = 0; i < forms.size(); ++i) {
                    bool is_last = (i + 1 == forms.size());
                    if (!compile_subset_expr(forms[i], code, constants, env, tail && is_last)) return false;
                    if (!is_last) code.push_back(OP_POP);
                }
                return true;
            }

            std::vector<std::pair<std::string, Object*>> rest_binds(binds.begin() + 1, binds.end());
            Object* nested_body_expr = nullptr;
            if (rest_binds.empty()) {
                std::vector<Object*> begin_items;
                begin_items.push_back(get_symbol("begin"));
                std::vector<Object*> body_forms = pair_to_vector(body_list);
                begin_items.insert(begin_items.end(), body_forms.begin(), body_forms.end());
                nested_body_expr = vector_to_pair(begin_items);
            } else {
                std::vector<Object*> rest_bind_nodes;
                for (auto& b : rest_binds) rest_bind_nodes.push_back(make_binding(b.first, b.second));
                Object* rest_bindings_expr = vector_to_pair(rest_bind_nodes);
                std::vector<Object*> letstar_items;
                letstar_items.push_back(get_symbol("let*"));
                letstar_items.push_back(rest_bindings_expr);
                std::vector<Object*> body_forms = pair_to_vector(body_list);
                letstar_items.insert(letstar_items.end(), body_forms.begin(), body_forms.end());
                nested_body_expr = vector_to_pair(letstar_items);
            }

            std::vector<Object*> one_binding = {make_binding(binds[0].first, binds[0].second)};
            Object* one_bindings_expr = vector_to_pair(one_binding);
            Object* transformed = vector_to_pair({get_symbol("let"), one_bindings_expr, nested_body_expr});
            return compile_subset_expr(transformed, code, constants, env, tail);
        }

        // letrec -> (let ((x nil) ...) (set! x e) ... body...)
        std::vector<Object*> init_bind_nodes;
        for (auto& b : binds) init_bind_nodes.push_back(make_binding(b.first, nullptr));
        Object* init_bindings_expr = vector_to_pair(init_bind_nodes);

        std::vector<Object*> letrec_body_items;
        letrec_body_items.push_back(get_symbol("let"));
        letrec_body_items.push_back(init_bindings_expr);
        for (auto& b : binds) {
            Object* set_form = vector_to_pair({get_symbol("set!"), get_symbol(b.first), b.second});
            letrec_body_items.push_back(set_form);
        }
        std::vector<Object*> original_forms = pair_to_vector(body_list);
        letrec_body_items.insert(letrec_body_items.end(), original_forms.begin(), original_forms.end());
        Object* transformed_letrec = vector_to_pair(letrec_body_items);
        return compile_subset_expr(transformed_letrec, code, constants, env, tail);
    }

    // --- and / or / cond / when / unless expansion in compiler ---
    if (is_symbol_name(head, "and")) {
        std::vector<Object*> forms = pair_to_vector(rest);
        if (forms.empty()) {
            code.push_back(OP_LDTRUE);
            return true;
        }
        if (forms.size() == 1) return compile_subset_expr(forms[0], code, constants, env, tail);
        // (and a b ...) -> (if a (and b ...) false)
        std::vector<Object*> tail_forms(forms.begin() + 1, forms.end());
        std::vector<Object*> and_tail = {get_symbol("and")};
        and_tail.insert(and_tail.end(), tail_forms.begin(), tail_forms.end());
        Object* transformed = vector_to_pair({
            get_symbol("if"), forms[0], vector_to_pair(and_tail), get_symbol("false")
        });
        return compile_subset_expr(transformed, code, constants, env, tail);
    }
    if (is_symbol_name(head, "or")) {
        std::vector<Object*> forms = pair_to_vector(rest);
        if (forms.empty()) {
            code.push_back(OP_LDFALSE);
            return true;
        }
        if (forms.size() == 1) return compile_subset_expr(forms[0], code, constants, env, tail);
        // (or a b ...) -> ((lambda (t) (if t t (or b ...))) a)
        std::vector<Object*> tail_forms(forms.begin() + 1, forms.end());
        std::vector<Object*> or_tail = {get_symbol("or")};
        or_tail.insert(or_tail.end(), tail_forms.begin(), tail_forms.end());
        Object* t_sym = get_symbol("_or_t_");
        Object* inner_if = vector_to_pair({get_symbol("if"), t_sym, t_sym, vector_to_pair(or_tail)});
        Object* lam = vector_to_pair({get_symbol("lambda"), vector_to_pair({t_sym}), inner_if});
        Object* transformed = vector_to_pair({lam, forms[0]});
        return compile_subset_expr(transformed, code, constants, env, tail);
    }
    if (is_symbol_name(head, "cond")) {
        // expand to nested ifs
        Object* cur = rest;
        if (!cur || cur->type != PAIR) {
            code.push_back(OP_LDNIL);
            return true;
        }
        Object* clause = cur->pair.car;
        Object* remaining = cur->pair.cdr;
        if (!clause || clause->type != PAIR) return false;
        Object* test = clause->pair.car;
        Object* body = clause->pair.cdr;
        if (is_symbol_name(test, "else")) {
            std::vector<Object*> body_forms = pair_to_vector(body);
            if (body_forms.empty()) { code.push_back(OP_LDNIL); return true; }
            Object* begin_form = vector_to_pair([&]() {
                std::vector<Object*> v = {get_symbol("begin")};
                v.insert(v.end(), body_forms.begin(), body_forms.end());
                return v;
            }());
            return compile_subset_expr(begin_form, code, constants, env, tail);
        }
        std::vector<Object*> body_forms = pair_to_vector(body);
        Object* then_part;
        if (body_forms.empty()) then_part = test;
        else {
            std::vector<Object*> begin_items = {get_symbol("begin")};
            begin_items.insert(begin_items.end(), body_forms.begin(), body_forms.end());
            then_part = vector_to_pair(begin_items);
        }
        Object* else_part = remaining
            ? vector_to_pair([&]() {
                std::vector<Object*> v = {get_symbol("cond")};
                std::vector<Object*> rest_clauses = pair_to_vector(remaining);
                v.insert(v.end(), rest_clauses.begin(), rest_clauses.end());
                return v;
            }())
            : nullptr;
        Object* transformed = else_part
            ? vector_to_pair({get_symbol("if"), test, then_part, else_part})
            : vector_to_pair({get_symbol("if"), test, then_part, nullptr});
        if (!else_part) {
            // (if test then) — use explicit nil for else
            if (!compile_subset_expr(test, code, constants, env)) return false;
            code.push_back(OP_JZ);
            int jz_idx = static_cast<int>(code.size());
            code.push_back(0);
            if (!compile_subset_expr(then_part, code, constants, env, tail)) return false;
            int end_pos = static_cast<int>(code.size());
            code[jz_idx] = end_pos - (jz_idx + 1);
            return true;
        }
        return compile_subset_expr(transformed, code, constants, env, tail);
    }
    if (is_symbol_name(head, "when")) {
        Object* test = (rest && rest->type == PAIR) ? rest->pair.car : nullptr;
        Object* body = (rest && rest->type == PAIR) ? rest->pair.cdr : nullptr;
        std::vector<Object*> body_forms = pair_to_vector(body);
        if (body_forms.empty()) { code.push_back(OP_LDNIL); return true; }
        std::vector<Object*> begin_items = {get_symbol("begin")};
        begin_items.insert(begin_items.end(), body_forms.begin(), body_forms.end());
        Object* transformed = vector_to_pair({get_symbol("if"), test, vector_to_pair(begin_items), nullptr});
        if (!compile_subset_expr(test, code, constants, env)) return false;
        code.push_back(OP_JZ);
        int jz_idx = static_cast<int>(code.size());
        code.push_back(0);
        if (!compile_subset_expr(vector_to_pair(begin_items), code, constants, env, tail)) return false;
        int end_pos = static_cast<int>(code.size());
        code[jz_idx] = end_pos - (jz_idx + 1);
        (void)transformed;
        return true;
    }
    if (is_symbol_name(head, "unless")) {
        Object* test = (rest && rest->type == PAIR) ? rest->pair.car : nullptr;
        Object* body = (rest && rest->type == PAIR) ? rest->pair.cdr : nullptr;
        std::vector<Object*> body_forms = pair_to_vector(body);
        if (body_forms.empty()) { code.push_back(OP_LDNIL); return true; }
        std::vector<Object*> begin_items = {get_symbol("begin")};
        begin_items.insert(begin_items.end(), body_forms.begin(), body_forms.end());
        // (unless test body) -> (if (not test) body)
        Object* not_test = vector_to_pair({get_symbol("not"), test});
        if (!compile_subset_expr(not_test, code, constants, env)) return false;
        code.push_back(OP_JZ);
        int jz_idx = static_cast<int>(code.size());
        code.push_back(0);
        if (!compile_subset_expr(vector_to_pair(begin_items), code, constants, env, tail)) return false;
        int end_pos = static_cast<int>(code.size());
        code[jz_idx] = end_pos - (jz_idx + 1);
        return true;
    }

    std::vector<Object*> args = pair_to_vector(rest);
    for (auto* a : args) {
        if (!compile_subset_expr(a, code, constants, env)) return false;
    }

    if (head && head->type == SYMBOL && head->sym) {
        int level = 0;
        int index = 0;
        if (!find_local(env, *head->sym, level, index) && globals.find(*head->sym) != globals.end()) {
            code.push_back(tail ? OP_TCALLG : OP_CALLG);
            code.push_back(add_constant(constants, head));
            code.push_back(static_cast<int>(args.size()));
            return true;
        }
    }

    if (!compile_subset_expr(head, code, constants, env)) return false;
    code.push_back(tail ? OP_TCALL : OP_CALL);
    code.push_back(static_cast<int>(args.size()));
    return true;
}

bool try_eval_expr_via_vm(Object* expr, Object*& out_value) {
    constexpr int OP_STOP = 15;

    std::vector<int> code;
    std::vector<Object*> constants;
    CompileEnv env;
    if (!compile_subset_expr(expr, code, constants, env)) return false;
    code.push_back(OP_STOP);

    VMContext vm_ctx;
    vm_ctx.c = std::move(code);
    out_value = vm(vm_ctx, constants);
    g_vm_subset_success_count++;
    return true;
}

Object* invoke_callable(Object* proc, const std::vector<Object*>& args) {
    if (!proc) return nullptr;
    if (proc->type == PRIMITIVE) {
        return proc->prim ? (*proc->prim)(const_cast<std::vector<Object*>&>(args)) : nullptr;
    }
    if (proc->type == CLOSURE && proc->closure.code && proc->closure.env) {
        if (proc->closure.constants && proc->closure.constants->size() >= 2 && proc->closure.code->empty()) {
            Object* params_expr = (*proc->closure.constants)[0];
            Object* body_list = (*proc->closure.constants)[1];
            ParamSpec spec;
            if (!extract_param_spec(params_expr, spec)) return nullptr;

            EvalEnv previous_env = g_eval_env_stack;
            auto it_env = g_closure_lexenv.find(proc);
            if (it_env != g_closure_lexenv.end()) {
                g_eval_env_stack = it_env->second;
            } else {
                g_eval_env_stack.clear();
            }

            g_eval_env_stack.push_back(EvalFrame{});
            EvalFrame& call_frame = g_eval_env_stack.back();
            if (!spec.has_rest && spec.fixed.size() != args.size()) {
                g_eval_env_stack = previous_env;
                return nullptr;
            }
            if (spec.has_rest && args.size() < spec.fixed.size()) {
                g_eval_env_stack = previous_env;
                return nullptr;
            }
            for (size_t i = 0; i < spec.fixed.size(); ++i) {
                call_frame[spec.fixed[i]] = args[i];
            }
            if (spec.has_rest) {
                std::vector<Object*> rest_items(args.begin() + static_cast<long long>(spec.fixed.size()), args.end());
                call_frame[spec.rest] = vector_to_pair(rest_items);
            }

            Object* result = nullptr;
            Object* cur = body_list;
            while (cur && cur->type == PAIR) {
                result = eval_expr(cur->pair.car);
                cur = cur->pair.cdr;
            }

            g_eval_env_stack = previous_env;
            return result;
        }

        VMContext call_ctx;
        call_ctx.e = *proc->closure.env;
        call_ctx.e.insert(call_ctx.e.begin(), args);
        call_ctx.c = *proc->closure.code;
        std::vector<Object*> call_constants = proc->closure.constants ? *proc->closure.constants : std::vector<Object*>{};
        return vm(call_ctx, call_constants);
    }
    if (proc->type == CONTINUATION && proc->continuation.s && proc->continuation.e && proc->continuation.c && proc->continuation.d) {
        VMContext cont_ctx;
        cont_ctx.s = *proc->continuation.s;
        cont_ctx.s.push_back(args.empty() ? nullptr : args[0]);
        cont_ctx.e = *proc->continuation.e;
        cont_ctx.c = *proc->continuation.c;
        cont_ctx.d = *proc->continuation.d;
        std::vector<Object*> cont_constants = proc->continuation.constants ? *proc->continuation.constants : std::vector<Object*>{};
        return vm(cont_ctx, cont_constants);
    }
    return nullptr;
}

Object* macro_expand_1(Object* expr) {
    if (!expr || expr->type != PAIR) return expr;
    Object* head = expr->pair.car;
    if (!head || head->type != SYMBOL || !head->sym) return expr;
    auto it = macros.find(*head->sym);
    if (it == macros.end() || !it->second) return expr;
    std::vector<Object*> raw_args = pair_to_vector(expr->pair.cdr);
    Object* expanded = invoke_callable(it->second, raw_args);
    return expanded ? expanded : expr;
}

Object* macro_expand(Object* expr) {
    Object* current = expr;
    while (true) {
        Object* expanded = macro_expand_1(current);
        if (expanded == current) return current;
        current = expanded;
    }
}

Object* lookup_name(const std::string& name) {
    for (auto it = g_eval_env_stack.rbegin(); it != g_eval_env_stack.rend(); ++it) {
        auto fit = it->find(name);
        if (fit != it->end()) return fit->second;
    }
    auto git = globals.find(name);
    if (git != globals.end()) return git->second;
    return nullptr;
}

void define_name(const std::string& name, Object* value) {
    if (!g_eval_env_stack.empty()) {
        g_eval_env_stack.back()[name] = value;
    } else {
        globals[name] = value;
    }
}

bool set_existing_name(const std::string& name, Object* value) {
    for (auto it = g_eval_env_stack.rbegin(); it != g_eval_env_stack.rend(); ++it) {
        auto fit = it->find(name);
        if (fit != it->end()) {
            fit->second = value;
            return true;
        }
    }
    auto git = globals.find(name);
    if (git != globals.end()) {
        git->second = value;
        return true;
    }
    return false;
}

bool parse_binding_list(Object* bindings_expr, std::vector<std::pair<std::string, Object*>>& out) {
    out.clear();
    Object* cur = bindings_expr;
    while (cur) {
        if (cur->type != PAIR || !cur->pair.car || cur->pair.car->type != PAIR) return false;
        Object* bind = cur->pair.car;
        Object* name_obj = bind->pair.car;
        Object* bind_tail = bind->pair.cdr;
        if (!name_obj || name_obj->type != SYMBOL || !name_obj->sym || !bind_tail || bind_tail->type != PAIR) return false;
        out.push_back({*name_obj->sym, bind_tail->pair.car});
        cur = cur->pair.cdr;
    }
    return true;
}

Object* make_eval_closure(Object* params_expr, Object* body_list) {
    ParamSpec spec;
    if (!extract_param_spec(params_expr, spec)) return nullptr;
    Object* clo = alloc(CLOSURE);
    clo->closure.code = new std::vector<int>();
    clo->closure.env = new std::vector<std::vector<Object*>>();
    clo->closure.constants = new std::vector<Object*>({params_expr, body_list});
    g_closure_lexenv[clo] = g_eval_env_stack;
    return clo;
}

bool lookup_lexical_now(const std::string& name, Object*& value) {
    for (auto it = g_eval_env_stack.rbegin(); it != g_eval_env_stack.rend(); ++it) {
        auto fit = it->find(name);
        if (fit != it->end()) {
            value = fit->second;
            return true;
        }
    }
    return false;
}

bool try_eval_call_via_vm(const std::string& global_name, const std::vector<Object*>& args, Object*& out_value) {
    auto it = globals.find(global_name);
    if (it == globals.end() || !it->second) return false;
    if (it->second->type != PRIMITIVE) return false;

    VMContext call_ctx;
    std::vector<Object*> constants;
    constants.reserve(args.size() + 1);
    for (auto* a : args) constants.push_back(a);
    Object* sym = get_symbol(global_name);
    if (!sym) return false;
    constants.push_back(sym);

    // Keep these aligned with enum Op.
    constexpr int OP_LDC = 1;
    constexpr int OP_STOP = 15;
    constexpr int OP_CALLG = 18;

    for (size_t i = 0; i < args.size(); ++i) {
        call_ctx.c.push_back(OP_LDC);
        call_ctx.c.push_back(static_cast<int>(i));
    }
    call_ctx.c.push_back(OP_CALLG);
    call_ctx.c.push_back(static_cast<int>(constants.size() - 1));
    call_ctx.c.push_back(static_cast<int>(args.size()));
    call_ctx.c.push_back(OP_STOP);

    out_value = vm(call_ctx, constants);
    return true;
}

bool is_symbol_name(Object* obj, const std::string& name) {
    return obj && obj->type == SYMBOL && obj->sym && *obj->sym == name;
}

bool is_tagged_form(Object* obj, const std::string& tag) {
    return obj && obj->type == PAIR && is_symbol_name(obj->pair.car, tag) && obj->pair.cdr &&
           obj->pair.cdr->type == PAIR && !obj->pair.cdr->pair.cdr;
}

Object* wrap_unary_form(const std::string& name, Object* body) {
    return cons(get_symbol(name), cons(body, nullptr));
}

Object* backquote_transfer(Object* expr) {
    if (!expr) {
        return vector_to_pair({get_symbol("quote"), nullptr});
    }
    if (expr->type != PAIR) {
        return vector_to_pair({get_symbol("quote"), expr});
    }

    std::vector<Object*> items = pair_to_vector(expr);
    if (items.size() == 2 && is_symbol_name(items[0], "unquote")) {
        return items[1];
    }

    Object* result = vector_to_pair({get_symbol("quote"), nullptr});
    for (auto it = items.rbegin(); it != items.rend(); ++it) {
        Object* item = *it;
        if (is_tagged_form(item, "unquote")) {
            Object* inner = item->pair.cdr->pair.car;
            result = vector_to_pair({get_symbol("cons"), inner, result});
        } else if (is_tagged_form(item, "splice")) {
            Object* inner = item->pair.cdr->pair.car;
            result = vector_to_pair({get_symbol("append"), inner, result});
        } else if (item && item->type == PAIR) {
            result = vector_to_pair({get_symbol("cons"), backquote_transfer(item), result});
        } else {
            result = vector_to_pair({get_symbol("cons"), vector_to_pair({get_symbol("quote"), item}), result});
        }
    }
    return result;
}

Object* eval_sequence(Object* body_list) {
    Object* result = nullptr;
    Object* cur = body_list;
    while (cur) {
        if (!cur || cur->type != PAIR) break;
        result = eval_expr(cur->pair.car);
        cur = cur->pair.cdr;
    }
    return result;
}

Object* eval_expr(Object* expr) {
    if (!expr) return nullptr;
    if (expr->type == INT || expr->type == PRIMITIVE || expr->type == CLOSURE || expr->type == CONTINUATION || expr->type == PORT) return expr;
    if (expr->type == SYMBOL) {
        Object* v = lookup_name(*expr->sym);
        if (v) return v;
        return expr;
    }
    if (expr->type != PAIR) return expr;

    Object* head = expr->pair.car;
    Object* rest = expr->pair.cdr;

    bool is_macro_call = false;
    if (head && head->type == SYMBOL && head->sym) {
        auto mit = macros.find(*head->sym);
        is_macro_call = (mit != macros.end() && mit->second != nullptr);
    }
    if (!is_macro_call && !is_symbol_name(head, "call/cc")) {
        Object* vm_out = nullptr;
        if (try_eval_expr_via_vm(expr, vm_out)) {
            return vm_out;
        }
    }

    if (is_symbol_name(head, "quote")) {
        return (rest && rest->type == PAIR) ? rest->pair.car : nullptr;
    }
    if (is_symbol_name(head, "backquote")) {
        Object* arg = (rest && rest->type == PAIR) ? rest->pair.car : nullptr;
        return eval_expr(backquote_transfer(arg));
    }
    if (is_symbol_name(head, "if")) {
        Object* test_expr = (rest && rest->type == PAIR) ? rest->pair.car : nullptr;
        Object* tail1 = (rest && rest->type == PAIR) ? rest->pair.cdr : nullptr;
        Object* then_expr = (tail1 && tail1->type == PAIR) ? tail1->pair.car : nullptr;
        Object* tail2 = (tail1 && tail1->type == PAIR) ? tail1->pair.cdr : nullptr;
        Object* else_expr = (tail2 && tail2->type == PAIR) ? tail2->pair.car : nullptr;
        Object* cond = eval_expr(test_expr);
        return is_true(cond) ? eval_expr(then_expr) : eval_expr(else_expr);
    }
    if (is_symbol_name(head, "begin")) {
        return eval_sequence(rest);
    }
    if (is_symbol_name(head, "lambda")) {
        Object* params_expr = (rest && rest->type == PAIR) ? rest->pair.car : nullptr;
        Object* body_list = (rest && rest->type == PAIR) ? rest->pair.cdr : nullptr;
        return make_eval_closure(params_expr, body_list);
    }
    if (is_symbol_name(head, "call/cc")) {
        Object* proc_expr = (rest && rest->type == PAIR) ? rest->pair.car : nullptr;
        Object* proc = eval_expr(proc_expr);
        if (!proc) return nullptr;

        Object* tag = alloc(UNDEF);
        Object* cont = alloc(PRIMITIVE);
        cont->prim = new std::function<Object*(std::vector<Object*>&)>([tag](std::vector<Object*>& args) -> Object* {
            Object* value = args.empty() ? nullptr : args[0];
            throw ContinuationEscape{tag, value};
        });

        try {
            std::vector<Object*> one_arg = {cont};
            return invoke_callable(proc, one_arg);
        } catch (const ContinuationEscape& esc) {
            if (esc.tag == tag) return esc.value;
            throw;
        }
    }
    if (is_symbol_name(head, "define")) {
        Object* name_obj = (rest && rest->type == PAIR) ? rest->pair.car : nullptr;
        Object* tail = (rest && rest->type == PAIR) ? rest->pair.cdr : nullptr;
        if (!name_obj) return nullptr;

        if (name_obj->type == SYMBOL && name_obj->sym) {
            Object* val_expr = (tail && tail->type == PAIR) ? tail->pair.car : nullptr;
            Object* val = eval_expr(val_expr);
            define_name(*name_obj->sym, val);
            return name_obj;
        }

        if (name_obj->type == PAIR && name_obj->pair.car && name_obj->pair.car->type == SYMBOL && name_obj->pair.car->sym) {
            Object* fname = name_obj->pair.car;
            Object* params_expr = name_obj->pair.cdr;
            Object* body_list = tail;
            Object* clo = make_eval_closure(params_expr, body_list);
            if (!clo) return nullptr;
            define_name(*fname->sym, clo);
            return fname;
        }
        return nullptr;
    }
    if (is_symbol_name(head, "set!")) {
        Object* name_obj = (rest && rest->type == PAIR) ? rest->pair.car : nullptr;
        Object* tail = (rest && rest->type == PAIR) ? rest->pair.cdr : nullptr;
        Object* val_expr = (tail && tail->type == PAIR) ? tail->pair.car : nullptr;
        if (!name_obj || name_obj->type != SYMBOL || !name_obj->sym) return nullptr;
        Object* val = eval_expr(val_expr);
        if (!set_existing_name(*name_obj->sym, val)) {
            globals[*name_obj->sym] = val;
        }
        return val;
    }
    if (is_symbol_name(head, "let") || is_symbol_name(head, "let*") || is_symbol_name(head, "letrec")) {
        Object* bindings_expr = (rest && rest->type == PAIR) ? rest->pair.car : nullptr;
        Object* body_list = (rest && rest->type == PAIR) ? rest->pair.cdr : nullptr;

        // named let: (let name ((var init) ...) body...)
        if (is_symbol_name(head, "let") && bindings_expr && bindings_expr->type == SYMBOL && bindings_expr->sym) {
            std::string loop_name = *bindings_expr->sym;
            Object* real_bindings = (body_list && body_list->type == PAIR) ? body_list->pair.car : nullptr;
            Object* real_body = (body_list && body_list->type == PAIR) ? body_list->pair.cdr : nullptr;
            std::vector<std::pair<std::string, Object*>> binds;
            if (!parse_binding_list(real_bindings, binds)) return nullptr;

            // build: (letrec ((name (lambda (vars...) body...))) (name inits...))
            std::vector<Object*> param_list;
            std::vector<Object*> init_list;
            for (auto& b : binds) {
                param_list.push_back(get_symbol(b.first));
                init_list.push_back(b.second);
            }
            Object* lam = cons(get_symbol("lambda"), cons(vector_to_pair(param_list), real_body));
            Object* letrec_bind = vector_to_pair({get_symbol(loop_name), lam});
            Object* letrec_bindings = cons(letrec_bind, nullptr);
            std::vector<Object*> call_items = {get_symbol(loop_name)};
            call_items.insert(call_items.end(), init_list.begin(), init_list.end());
            Object* call_expr = vector_to_pair(call_items);
            Object* letrec_expr = vector_to_pair({get_symbol("letrec"), letrec_bindings, call_expr});
            return eval_expr(letrec_expr);
        }

        std::vector<std::pair<std::string, Object*>> binds;
        if (!parse_binding_list(bindings_expr, binds)) return nullptr;

        if (is_symbol_name(head, "let")) {
            std::vector<Object*> values;
            for (auto& b : binds) values.push_back(eval_expr(b.second));
            g_eval_env_stack.push_back(EvalFrame{});
            for (size_t i = 0; i < binds.size(); ++i) g_eval_env_stack.back()[binds[i].first] = values[i];
            Object* result = eval_sequence(body_list);
            g_eval_env_stack.pop_back();
            return result;
        }

        if (is_symbol_name(head, "let*")) {
            g_eval_env_stack.push_back(EvalFrame{});
            for (auto& b : binds) {
                g_eval_env_stack.back()[b.first] = eval_expr(b.second);
            }
            Object* result = eval_sequence(body_list);
            g_eval_env_stack.pop_back();
            return result;
        }

        g_eval_env_stack.push_back(EvalFrame{});
        for (auto& b : binds) g_eval_env_stack.back()[b.first] = nullptr;
        for (auto& b : binds) g_eval_env_stack.back()[b.first] = eval_expr(b.second);
        Object* result = eval_sequence(body_list);
        g_eval_env_stack.pop_back();
        return result;
    }

    // --- and / or / cond / when / unless / case ---
    if (is_symbol_name(head, "and")) {
        std::vector<Object*> forms = pair_to_vector(rest);
        if (forms.empty()) return globals["true"];
        if (forms.size() == 1) return eval_expr(forms[0]);
        for (size_t i = 0; i + 1 < forms.size(); ++i) {
            Object* v = eval_expr(forms[i]);
            if (!is_true(v)) return globals["false"];
        }
        return eval_expr(forms.back());
    }
    if (is_symbol_name(head, "or")) {
        std::vector<Object*> forms = pair_to_vector(rest);
        if (forms.empty()) return globals["false"];
        if (forms.size() == 1) return eval_expr(forms[0]);
        for (size_t i = 0; i + 1 < forms.size(); ++i) {
            Object* v = eval_expr(forms[i]);
            if (is_true(v)) return v;
        }
        return eval_expr(forms.back());
    }
    if (is_symbol_name(head, "cond")) {
        Object* cur = rest;
        while (cur && cur->type == PAIR) {
            Object* clause = cur->pair.car;
            cur = cur->pair.cdr;
            if (!clause || clause->type != PAIR) continue;
            Object* test = clause->pair.car;
            Object* body = clause->pair.cdr;
            if (is_symbol_name(test, "else")) {
                return eval_sequence(body);
            }
            Object* tv = eval_expr(test);
            if (is_true(tv)) {
                if (!body) return tv;
                return eval_sequence(body);
            }
        }
        return nullptr;
    }
    if (is_symbol_name(head, "when")) {
        Object* test = (rest && rest->type == PAIR) ? rest->pair.car : nullptr;
        Object* body = (rest && rest->type == PAIR) ? rest->pair.cdr : nullptr;
        if (is_true(eval_expr(test))) return eval_sequence(body);
        return nullptr;
    }
    if (is_symbol_name(head, "unless")) {
        Object* test = (rest && rest->type == PAIR) ? rest->pair.car : nullptr;
        Object* body = (rest && rest->type == PAIR) ? rest->pair.cdr : nullptr;
        if (!is_true(eval_expr(test))) return eval_sequence(body);
        return nullptr;
    }
    if (is_symbol_name(head, "case")) {
        Object* key_expr = (rest && rest->type == PAIR) ? rest->pair.car : nullptr;
        Object* key = eval_expr(key_expr);
        Object* cur = (rest && rest->type == PAIR) ? rest->pair.cdr : nullptr;
        while (cur && cur->type == PAIR) {
            Object* clause = cur->pair.car;
            cur = cur->pair.cdr;
            if (!clause || clause->type != PAIR) continue;
            Object* datums = clause->pair.car;
            Object* body = clause->pair.cdr;
            if (is_symbol_name(datums, "else")) return eval_sequence(body);
            for (Object* d = datums; d && d->type == PAIR; d = d->pair.cdr) {
                if (objects_equal(key, d->pair.car)) return eval_sequence(body);
            }
        }
        return nullptr;
    }

    // do: (do ((var init step) ...) (test result...) body...)
    if (is_symbol_name(head, "do")) {
        Object* var_form = (rest && rest->type == PAIR) ? rest->pair.car : nullptr;
        Object* rest2 = (rest && rest->type == PAIR) ? rest->pair.cdr : nullptr;
        Object* test_form = (rest2 && rest2->type == PAIR) ? rest2->pair.car : nullptr;
        Object* body = (rest2 && rest2->type == PAIR) ? rest2->pair.cdr : nullptr;

        // parse var-form: each binding is (var init) or (var init step)
        std::vector<Object*> vars, inits, steps;
        for (Object* cur = var_form; cur && cur->type == PAIR; cur = cur->pair.cdr) {
            Object* b = cur->pair.car;
            std::vector<Object*> bv = pair_to_vector(b);
            if (bv.empty()) continue;
            vars.push_back(bv[0]);
            inits.push_back(bv.size() > 1 ? bv[1] : nullptr);
            steps.push_back(bv.size() > 2 ? bv[2] : bv[0]);
        }

        Object* test_expr = (test_form && test_form->type == PAIR) ? test_form->pair.car : nullptr;
        Object* result_forms = (test_form && test_form->type == PAIR) ? test_form->pair.cdr : nullptr;

        // build: (letrec ((loop (lambda (vars...) (if test result (begin body... (loop steps...))))))
        //           (loop inits...))
        std::string loop_sym = "_do_loop_";
        std::vector<Object*> param_syms;
        for (auto* v : vars) param_syms.push_back(v);

        std::vector<Object*> step_call = {get_symbol(loop_sym)};
        for (auto* s : steps) step_call.push_back(s);

        // then-part: (begin result...) or nil
        Object* then_part;
        std::vector<Object*> rfv = pair_to_vector(result_forms);
        if (rfv.empty()) then_part = nullptr;
        else if (rfv.size() == 1) then_part = rfv[0];
        else {
            std::vector<Object*> begin_items = {get_symbol("begin")};
            begin_items.insert(begin_items.end(), rfv.begin(), rfv.end());
            then_part = vector_to_pair(begin_items);
        }

        // else-part: (begin body... (loop steps...))
        std::vector<Object*> body_forms = pair_to_vector(body);
        std::vector<Object*> else_items;
        else_items.push_back(get_symbol("begin"));
        else_items.insert(else_items.end(), body_forms.begin(), body_forms.end());
        else_items.push_back(vector_to_pair(step_call));
        Object* else_part = vector_to_pair(else_items);

        Object* if_expr = vector_to_pair({
            get_symbol("if"), test_expr,
            then_part ? then_part : (Object*)nullptr,
            else_part
        });

        Object* lam = cons(get_symbol("lambda"), cons(vector_to_pair(param_syms), cons(if_expr, nullptr)));
        Object* letrec_bind = vector_to_pair({get_symbol(loop_sym), lam});
        Object* letrec_bindings = cons(letrec_bind, nullptr);
        std::vector<Object*> call_items = {get_symbol(loop_sym)};
        call_items.insert(call_items.end(), inits.begin(), inits.end());
        Object* letrec_expr = vector_to_pair({
            get_symbol("letrec"), letrec_bindings, vector_to_pair(call_items)
        });
        return eval_expr(letrec_expr);
    }

    if (is_symbol_name(head, "define-macro")) {
        Object* name_obj = (rest && rest->type == PAIR) ? rest->pair.car : nullptr;
        Object* tail = (rest && rest->type == PAIR) ? rest->pair.cdr : nullptr;
        if (!name_obj) return nullptr;

        if (name_obj->type == SYMBOL && name_obj->sym) {
            Object* val_expr = (tail && tail->type == PAIR) ? tail->pair.car : nullptr;
            Object* val = eval_expr(val_expr);
            if (!val || (val->type != PRIMITIVE && val->type != CLOSURE && val->type != CONTINUATION)) {
                return nullptr;
            }
            macros[*name_obj->sym] = val;
            return name_obj;
        }

        if (name_obj->type == PAIR && name_obj->pair.car && name_obj->pair.car->type == SYMBOL && name_obj->pair.car->sym) {
            Object* mname = name_obj->pair.car;
            Object* params_expr = name_obj->pair.cdr;
            Object* body_list = tail;
            Object* clo = make_eval_closure(params_expr, body_list);
            if (!clo) return nullptr;
            macros[*mname->sym] = clo;
            return mname;
        }
        return nullptr;
    }

    if (head && head->type == SYMBOL && head->sym) {
        auto mit = macros.find(*head->sym);
        if (mit != macros.end() && mit->second) {
            std::vector<Object*> raw_args = pair_to_vector(rest);
            for (auto* a : raw_args) g_active_ctx->s.push_back(a);
            Object* expanded = invoke_callable(mit->second, raw_args);
            for (size_t i = 0; i < raw_args.size(); ++i) g_active_ctx->s.pop_back();
            return eval_expr(expanded ? expanded : expr);
        }
    }

    std::string head_symbol_name;
    if (head && head->type == SYMBOL && head->sym) {
        head_symbol_name = *head->sym;
    }

    Object* proc = eval_expr(head);
    std::vector<Object*> evaled_args;
    for (Object* cur = rest; cur && cur->type == PAIR; cur = cur->pair.cdr) {
        Object* av = eval_expr(cur->pair.car);
        evaled_args.push_back(av);
    }

    g_active_ctx->s.push_back(proc);
    for (auto* a : evaled_args) g_active_ctx->s.push_back(a);
    Object* result = nullptr;
    if (!head_symbol_name.empty()) {
        Object* vm_result = nullptr;
        if (try_eval_call_via_vm(head_symbol_name, evaled_args, vm_result)) {
            result = vm_result;
        }
    }
    if (!result) {
        result = invoke_callable(proc, evaled_args);
    }
    for (size_t i = 0; i < evaled_args.size() + 1; ++i) g_active_ctx->s.pop_back();
    return result;
}

enum Op { 
    LD,
    LDC,
    LDG,
    LDF,
    LDCT,
    LSET,
    GSET,
    APP,
    TAPP,
    RTN,
    POP,
    ARGS,
    ARGS_AP,// Apply with args
    DEF,
    DEFM,
    STOP,
    CALL,
    TCALL,
    CALLG,
    TCALLG,
    LDNIL,
    LDTRUE,
    LDFALSE,
    ADDI,
    SUBI,
    MULI,
    EQI,
    LTI,
    GTI,
    LEI,
    GEI,
    JZ,
    JMP,
    CALLCC,
    SEL,
    SELR,
    JOIN
};

// ------------------------------------------------------------
// VM: SECD 仮想マシン本体
//
// ctx.c に格納されたバイトコード列を pc=0 から実行し、
// STOP 命令または RTN でダンプが空になったときに ctx.s の
// スタックトップを返す。
//
// 主な実行ループの動作:
//   - LDC/LDG/LD 系  : 値をスタックに積む
//   - CALL/TCALL     : クロージャ呼び出し
//                      CALL はダンプに {s,e,c,constants} を保存する
//                      TCALL はダンプに保存しない（末尾呼び出し最適化）
//   - RTN            : ダンプを pop してスタックトップを呼び出し元へ渡す
//   - JZ/JMP         : 条件・無条件ジャンプ（if の実装）
//   - DEF/GSET/LSET  : 変数への代入
//   - CALLCC         : 現在の継続を CONTINUATION オブジェクトとして捕捉
//
// g_active_ctx と g_active_constants を更新することで、
// VM 実行中に alloc が GC を正しく呼べるようにする。
// ------------------------------------------------------------
Object* vm(VMContext& ctx, std::vector<Object*>& constants) {
    VMContext* prev_ctx = g_active_ctx;
    std::vector<Object*>* prev_constants = g_active_constants;
    g_active_ctx = &ctx;
    g_active_constants = &constants;

    size_t pc = 0;

    while (pc < ctx.c.size()) {
        Op cmd = static_cast<Op>(ctx.c[pc++]);
        switch (cmd) {
            case LDC:
                ctx.s.push_back(constants[static_cast<size_t>(ctx.c[pc++])]);
                break;
            case LDNIL:
                ctx.s.push_back(nullptr);
                break;
            case LDTRUE:
                ctx.s.push_back(globals["true"]);
                break;
            case LDFALSE:
                ctx.s.push_back(globals["false"]);
                break;
            case ADDI:
            case SUBI:
            case MULI:
            case EQI:
            case LTI:
            case GTI:
            case LEI:
            case GEI: {
                if (ctx.s.size() < 2) {
                    break;
                }
                Object* right = ctx.s.back();
                ctx.s.pop_back();
                Object* left = ctx.s.back();
                ctx.s.pop_back();

                if (left && right && left->type == INT && right->type == INT) {
                    if (cmd == ADDI) {
                        if (will_overflow_add(left->num, right->num)) {
                            ctx.s.push_back(make_number((long long)left->num + (long long)right->num));
                        } else {
                            ctx.s.push_back(make_int(left->num + right->num));
                        }
                    }
                    else if (cmd == SUBI) {
                        if (will_overflow_sub(left->num, right->num)) {
                            ctx.s.push_back(make_number((long long)left->num - (long long)right->num));
                        } else {
                            ctx.s.push_back(make_int(left->num - right->num));
                        }
                    }
                    else if (cmd == MULI) {
                        if (will_overflow_mul(left->num, right->num)) {
                            ctx.s.push_back(make_number((long long)left->num * (long long)right->num));
                        } else {
                            ctx.s.push_back(make_int(left->num * right->num));
                        }
                    }
                    else if (cmd == EQI) ctx.s.push_back(bool_obj(left->num == right->num));
                    else if (cmd == LTI) ctx.s.push_back(bool_obj(left->num < right->num));
                    else if (cmd == GTI) ctx.s.push_back(bool_obj(left->num > right->num));
                    else if (cmd == LEI) ctx.s.push_back(bool_obj(left->num <= right->num));
                    else ctx.s.push_back(bool_obj(left->num >= right->num));
                } else {
                    std::string fallback;
                    if (cmd == ADDI) fallback = "+";
                    else if (cmd == SUBI) fallback = "-";
                    else if (cmd == MULI) fallback = "*";
                    else if (cmd == EQI) fallback = "=";
                    else if (cmd == LTI) fallback = "<";
                    else if (cmd == GTI) fallback = ">";
                    else if (cmd == LEI) fallback = "<=";
                    else fallback = ">=";

                    auto it = globals.find(fallback);
                    if (it != globals.end() && it->second && it->second->type == PRIMITIVE && it->second->prim) {
                        std::vector<Object*> args = {left, right};
                        ctx.s.push_back((*it->second->prim)(args));
                    } else {
                        ctx.s.push_back(nullptr);
                    }
                }
                break;
            }
            case LDG:
                ctx.s.push_back(globals[*constants[static_cast<size_t>(ctx.c[pc++])]->sym]);
                break;
            case LD: {
                size_t i = static_cast<size_t>(ctx.c[pc++]);
                size_t j = static_cast<size_t>(ctx.c[pc++]);
                ctx.s.push_back(get_lvar(ctx.e, i, j));
                break;
            }
            case LDF: {
                size_t idx = static_cast<size_t>(ctx.c[pc++]);
                Object* clo = alloc(CLOSURE);
                clo->closure.code = new std::vector<int>(*constants[idx]->closure.code);
                clo->closure.env = new std::vector<std::vector<Object*>>(ctx.e);
                clo->closure.constants = new std::vector<Object*>(constants);
                ctx.s.push_back(clo);
                break;
            }
            case LDCT: {
                Object* cont = alloc(CONTINUATION);
                cont->continuation.s = new std::vector<Object*>(ctx.s);
                cont->continuation.e = new std::vector<std::vector<Object*>>(ctx.e);
                cont->continuation.c = new std::vector<int>(ctx.c.begin() + pc, ctx.c.end());
                cont->continuation.d = new std::vector<DumpFrame>(ctx.d);
                cont->continuation.constants = new std::vector<Object*>(constants);
                ctx.s.push_back(cont);
                break;
            }
            case APP: {
                if (ctx.s.empty()) break;
                Object* clo = ctx.s.back(); 
                ctx.s.pop_back(); 
                
                int nargs = ctx.c[pc++];

                if (clo->type == PRIMITIVE) {
                    std::vector<Object*> prim_args;
                    for (int i = 0; i < nargs; ++i) {
                        prim_args.push_back(ctx.s[ctx.s.size() - nargs + i]);
                    }

                    Object* result = (*clo->prim)(prim_args);

                    for (int i = 0; i < nargs; ++i) ctx.s.pop_back();
                    
                    ctx.s.push_back(result);

                } else if (clo->type == CLOSURE) {
                    std::vector<Object*> args;
                    for (int i = 0; i < nargs; ++i) {
                        args.push_back(ctx.s.back());
                        ctx.s.pop_back();
                    }
                    std::reverse(args.begin(), args.end());

                    ctx.d.push_back({ctx.s, ctx.e, std::vector<int>(ctx.c.begin() + pc, ctx.c.end()), constants});
                    
                    ctx.s.clear();
                    ctx.e = *clo->closure.env;
                    ctx.e.insert(ctx.e.begin(), args);
                    ctx.c = *clo->closure.code;
                    if (clo->closure.constants) constants = *clo->closure.constants;
                    pc = 0;

                } else if (clo->type == CONTINUATION) {
                    Object* cont_val = (nargs > 0) ? ctx.s.back() : nullptr;
                    for (int i = 0; i < nargs; ++i) ctx.s.pop_back();

                    ctx.s = *clo->continuation.s;
                    ctx.s.push_back(cont_val);
                    ctx.e = *clo->continuation.e;
                    ctx.c = *clo->continuation.c;
                    ctx.d = *clo->continuation.d;
                    if (clo->continuation.constants) constants = *clo->continuation.constants;
                    pc = 0;
                }
                break;
            }
            case LSET: {
                size_t i = static_cast<size_t>(ctx.c[pc++]);
                size_t j = static_cast<size_t>(ctx.c[pc++]);
                if (!ctx.s.empty()) {
                    set_lvar(ctx.e, i, j, ctx.s.back());
                }
                break;
            }
            case GSET: {
                size_t sidx = static_cast<size_t>(ctx.c[pc++]);
                if (sidx < constants.size() && constants[sidx] && constants[sidx]->type == SYMBOL && !ctx.s.empty()) {
                    globals[*constants[sidx]->sym] = ctx.s.back();
                }
                break;
            }
            case CALL:
            case TCALL: {
                if (ctx.s.empty()) break;
                int nargs = ctx.c[pc++];
                if (ctx.s.size() < static_cast<size_t>(nargs + 1)) break;

                Object* clo = ctx.s.back();
                ctx.s.pop_back();

                std::vector<Object*> args;
                for (int i = 0; i < nargs; ++i) {
                    args.push_back(ctx.s.back());
                    ctx.s.pop_back();
                }
                std::reverse(args.begin(), args.end());

                if (!clo) {
                    ctx.s.push_back(nullptr);
                    break;
                }

                if (clo->type == PRIMITIVE) {
                    Object* result = clo->prim ? (*clo->prim)(args) : nullptr;
                    ctx.s.push_back(result);
                } else if (clo->type == CLOSURE) {
                    if (cmd == CALL) {
                        ctx.d.push_back({ctx.s, ctx.e, std::vector<int>(ctx.c.begin() + pc, ctx.c.end()), constants});
                        ctx.s.clear();
                    }
                    ctx.e = *clo->closure.env;
                    ctx.e.insert(ctx.e.begin(), args);
                    ctx.c = *clo->closure.code;
                    if (clo->closure.constants) constants = *clo->closure.constants;
                    pc = 0;
                } else if (clo->type == CONTINUATION) {
                    Object* cont_val = args.empty() ? nullptr : args[0];
                    ctx.s = *clo->continuation.s;
                    ctx.s.push_back(cont_val);
                    ctx.e = *clo->continuation.e;
                    ctx.c = *clo->continuation.c;
                    ctx.d = *clo->continuation.d;
                    if (clo->continuation.constants) constants = *clo->continuation.constants;
                    pc = 0;
                }
                break;
            }
            case CALLG:
            case TCALLG: {
                size_t sidx = static_cast<size_t>(ctx.c[pc++]);
                int nargs = ctx.c[pc++];
                if (sidx >= constants.size() || !constants[sidx] || constants[sidx]->type != SYMBOL) {
                    ctx.s.push_back(nullptr);
                    break;
                }
                std::string name = *constants[sidx]->sym;
                auto it = globals.find(name);
                if (it == globals.end()) {
                    ctx.s.push_back(nullptr);
                    break;
                }
                Object* clo = it->second;

                if (ctx.s.size() < static_cast<size_t>(nargs)) break;
                std::vector<Object*> args;
                for (int i = 0; i < nargs; ++i) {
                    args.push_back(ctx.s.back());
                    ctx.s.pop_back();
                }
                std::reverse(args.begin(), args.end());

                if (!clo) {
                    ctx.s.push_back(nullptr);
                    break;
                }
                if (clo->type == PRIMITIVE) {
                    Object* result = clo->prim ? (*clo->prim)(args) : nullptr;
                    ctx.s.push_back(result);
                } else if (clo->type == CLOSURE) {
                    if (cmd == CALLG) {
                        ctx.d.push_back({ctx.s, ctx.e, std::vector<int>(ctx.c.begin() + pc, ctx.c.end()), constants});
                        ctx.s.clear();
                    }
                    ctx.e = *clo->closure.env;
                    ctx.e.insert(ctx.e.begin(), args);
                    ctx.c = *clo->closure.code;
                    if (clo->closure.constants) constants = *clo->closure.constants;
                    pc = 0;
                } else if (clo->type == CONTINUATION) {
                    Object* cont_val = args.empty() ? nullptr : args[0];
                    ctx.s = *clo->continuation.s;
                    ctx.s.push_back(cont_val);
                    ctx.e = *clo->continuation.e;
                    ctx.c = *clo->continuation.c;
                    ctx.d = *clo->continuation.d;
                    if (clo->continuation.constants) constants = *clo->continuation.constants;
                    pc = 0;
                }
                break;
            }
            case ARGS: {
                int n = ctx.c[pc++];
                if (n < 0 || ctx.s.size() < static_cast<size_t>(n)) break;
                std::vector<Object*> items;
                for (int i = 0; i < n; ++i) {
                    items.push_back(ctx.s.back());
                    ctx.s.pop_back();
                }
                std::reverse(items.begin(), items.end());
                ctx.s.push_back(vector_to_pair(items));
                break;
            }
            case ARGS_AP: {
                int n = ctx.c[pc++];
                if (n <= 0 || ctx.s.size() < static_cast<size_t>(n)) break;
                Object* tail_list = ctx.s.back();
                ctx.s.pop_back();
                std::vector<Object*> tail_items = pair_to_vector(tail_list);
                std::vector<Object*> prefix;
                for (int i = 0; i < n - 1; ++i) {
                    prefix.push_back(ctx.s.back());
                    ctx.s.pop_back();
                }
                std::reverse(prefix.begin(), prefix.end());
                prefix.insert(prefix.end(), tail_items.begin(), tail_items.end());
                ctx.s.push_back(vector_to_pair(prefix));
                break;
            }
            case POP:
                if (!ctx.s.empty()) ctx.s.pop_back();
                break;
            case SEL: {
                int t_operand = ctx.c[pc++];
                int f_operand = ctx.c[pc++];
                if (ctx.s.empty()) break;
                Object* cond = ctx.s.back();
                ctx.s.pop_back();
                std::vector<int> t_clause = resolve_clause_operand(t_operand, constants);
                std::vector<int> f_clause = resolve_clause_operand(f_operand, constants);
                ctx.d.push_back({{}, {}, std::vector<int>(ctx.c.begin() + pc, ctx.c.end()), constants});
                ctx.c = (cond == globals["false"]) ? f_clause : t_clause;
                pc = 0;
                break;
            }
            case SELR: {
                int t_operand = ctx.c[pc++];
                int f_operand = ctx.c[pc++];
                if (ctx.s.empty()) break;
                Object* cond = ctx.s.back();
                ctx.s.pop_back();
                std::vector<int> t_clause = resolve_clause_operand(t_operand, constants);
                std::vector<int> f_clause = resolve_clause_operand(f_operand, constants);
                ctx.c = (cond == globals["false"]) ? f_clause : t_clause;
                pc = 0;
                break;
            }
            case JOIN: {
                if (ctx.d.empty()) break;
                DumpFrame df = ctx.d.back();
                ctx.d.pop_back();
                ctx.c = df.c;
                constants = df.constants;
                pc = 0;
                break;
            }
            case JZ: {
                int offset = ctx.c[pc++];
                if (ctx.s.empty()) break;
                Object* cond = ctx.s.back();
                ctx.s.pop_back();
                if (cond == globals["false"]) {
                    pc = static_cast<size_t>(static_cast<int>(pc) + offset);
                }
                break;
            }
            case JMP: {
                int offset = ctx.c[pc++];
                pc = static_cast<size_t>(static_cast<int>(pc) + offset);
                break;
            }
            case RTN: {
                if (ctx.d.empty()) goto end_vm;
                Object* ret = ctx.s.back();
                DumpFrame df = ctx.d.back(); ctx.d.pop_back();
                ctx.s = df.s;
                ctx.s.push_back(ret);
                ctx.e = df.e;
                ctx.c = df.c;
                constants = df.constants;
                pc = 0;
                break;
            }
            case DEF:
            case DEFM: {
                size_t sidx = static_cast<size_t>(ctx.c[pc++]);
                if (ctx.s.empty()) break;
                Object* value = ctx.s.back();
                ctx.s.pop_back();
                if (sidx < constants.size() && constants[sidx] && constants[sidx]->type == SYMBOL) {
                    Object* sym = constants[sidx];
                    if (cmd == DEFM) {
                        macros[*sym->sym] = value;
                    } else {
                        globals[*sym->sym] = value;
                    }
                    ctx.s.push_back(sym);
                } else {
                    ctx.s.push_back(nullptr);
                }
                break;
            }
            case CALLCC: {
                if (ctx.s.empty()) break;
                Object* fn = ctx.s.back();
                ctx.s.pop_back();

                Object* cont = alloc(CONTINUATION);
                cont->continuation.s = new std::vector<Object*>(ctx.s);
                cont->continuation.e = new std::vector<std::vector<Object*>>(ctx.e);
                cont->continuation.c = new std::vector<int>(ctx.c.begin() + pc, ctx.c.end());
                cont->continuation.d = new std::vector<DumpFrame>(ctx.d);
                cont->continuation.constants = new std::vector<Object*>(constants);

                std::vector<Object*> fn_args = {cont};
                if (!fn) { ctx.s.push_back(nullptr); break; }
                if (fn->type == PRIMITIVE) {
                    Object* r = fn->prim ? (*fn->prim)(fn_args) : nullptr;
                    ctx.s.push_back(r);
                } else if (fn->type == CLOSURE) {
                    ctx.d.push_back({ctx.s, ctx.e, std::vector<int>(ctx.c.begin() + pc, ctx.c.end()), constants});
                    ctx.s.clear();
                    ctx.e = *fn->closure.env;
                    ctx.e.insert(ctx.e.begin(), fn_args);
                    ctx.c = *fn->closure.code;
                    if (fn->closure.constants) constants = *fn->closure.constants;
                    pc = 0;
                } else if (fn->type == CONTINUATION) {
                    Object* cont_val = nullptr;
                    ctx.s = *fn->continuation.s;
                    ctx.s.push_back(cont_val);
                    ctx.e = *fn->continuation.e;
                    ctx.c = *fn->continuation.c;
                    ctx.d = *fn->continuation.d;
                    if (fn->continuation.constants) constants = *fn->continuation.constants;
                    pc = 0;
                }
                break;
            }
            case STOP: goto end_vm;
            default: break;
        }
    }

end_vm:
    g_active_ctx = prev_ctx;
    g_active_constants = prev_constants;
    return ctx.s.empty() ? nullptr : ctx.s.back();
}

// ------------------------------------------------------------
// トークナイザ・リーダ
//
// tokenize(s) : 文字列を Token のリストに変換する
// s_read      : トークン列から Scheme オブジェクトを1つ読み取る
// s_read_list : ( ... ) の内側を再帰的に読み取る
// s_input     : REPL 用の複数行入力（括弧が閉じるまで読み続ける）
//
// トークン種別:
//   LPAREN/RPAREN : ( )
//   DOT           : .  (ドット対 (a . b) 用)
//   INT           : 整数リテラル
//   SYM           : シンボル
//   QUOTE         : '  → (quote ...)
//   BACKQUOTE     : `  → (backquote ...)
//   UNQUOTE       : ,  → (unquote ...)
//   SPLICE        : ,@ → (splice ...)
//   END           : 入力終端
// ------------------------------------------------------------
enum TokenType { TOK_LPAREN, TOK_RPAREN, TOK_DOT, TOK_INT, TOK_SYM, TOK_QUOTE, TOK_BACKQUOTE, TOK_UNQUOTE, TOK_SPLICE, TOK_END };
struct Token {
    TokenType type;
    std::string str;
};

bool is_int(const std::string& s) {
    if (s.empty() || ((!isdigit(s[0])) && (s[0] != '-') && (s[0] != '+'))) return false;
    char * p;
    strtol(s.c_str(), &p, 10);
    return (*p == 0);
}

std::vector<Token> tokenize(std::string s) {
    std::vector<Token> tokens;
    for (size_t i = 0; i < s.size(); ) {
        if (isspace(s[i])) { i++; continue; }
        if (s[i] == ';') {
            while (i < s.size() && s[i] != '\n') i++;
            continue;
        }
        if (s[i] == '(') { tokens.push_back({TOK_LPAREN, "("}); i++; }
        else if (s[i] == ')') { tokens.push_back({TOK_RPAREN, ")"}); i++; }
        else if (s[i] == '\'') { tokens.push_back({TOK_QUOTE, "'"}); i++; }
        else if (s[i] == '`') { tokens.push_back({TOK_BACKQUOTE, "`"}); i++; }
        else if (s[i] == ',') {
            if (i + 1 < s.size() && s[i + 1] == '@') {
                tokens.push_back({TOK_SPLICE, ",@"});
                i += 2;
            } else {
                tokens.push_back({TOK_UNQUOTE, ","});
                i++;
            }
        }
        else {
            size_t start = i;
            while (i < s.size() && !isspace(s[i]) && s[i] != '(' && s[i] != ')') i++;
            std::string word = s.substr(start, i - start);
            if (word == ".") tokens.push_back({TOK_DOT, "."});
            else if (is_int(word)) tokens.push_back({TOK_INT, word});
            else tokens.push_back({TOK_SYM, word});
        }
    }
    tokens.push_back({TOK_END, ""});
    return tokens;
}


Object* s_read(const std::vector<Token>& tokens, int& idx);

Object* s_read_list(const std::vector<Token>& tokens, int& idx) {
    if (tokens[idx].type == TOK_RPAREN) {
        idx++;
        return nullptr;
    }

    Object* car = s_read(tokens, idx);
    if (g_active_ctx) g_active_ctx->s.push_back(car);

    if (tokens[idx].type == TOK_DOT) {
        idx++;
        Object* cdr = s_read(tokens, idx);
        if (g_active_ctx) g_active_ctx->s.push_back(cdr);

        Object* res = cons(car, cdr);

        if (g_active_ctx) {
            g_active_ctx->s.pop_back();
            g_active_ctx->s.pop_back();
        }
        if (tokens[idx].type == TOK_RPAREN) idx++;
        return res;
    }

    Object* cdr = s_read_list(tokens, idx);
    if (g_active_ctx) g_active_ctx->s.push_back(cdr);

    Object* res = cons(car, cdr);

    if (g_active_ctx) {
        g_active_ctx->s.pop_back();
        g_active_ctx->s.pop_back();
    }

    return res;
}

Object* s_read(const std::vector<Token>& tokens, int& idx) {
    if (tokens[idx].type == TOK_LPAREN) {
        idx++;
        return s_read_list(tokens, idx);
    } 
    
    if (tokens[idx].type == TOK_QUOTE) {
        idx++;
        Object* quoted_val = s_read(tokens, idx);
        if (g_active_ctx) g_active_ctx->s.push_back(quoted_val);
        
        Object* inner = cons(quoted_val, nullptr);
        if (g_active_ctx) g_active_ctx->s.push_back(inner);
        
        Object* res = cons(get_symbol("quote"), inner);
        
        if (g_active_ctx) {
            g_active_ctx->s.pop_back();
            g_active_ctx->s.pop_back();
        }
        return res;
    }

    if (tokens[idx].type == TOK_BACKQUOTE) {
        idx++;
        return wrap_unary_form("backquote", s_read(tokens, idx));
    }

    if (tokens[idx].type == TOK_UNQUOTE) {
        idx++;
        return wrap_unary_form("unquote", s_read(tokens, idx));
    }

    if (tokens[idx].type == TOK_SPLICE) {
        idx++;
        return wrap_unary_form("splice", s_read(tokens, idx));
    }

    if (tokens[idx].type == TOK_INT) {
        return make_int(std::stoi(tokens[idx++].str));
    } else if (tokens[idx].type == TOK_SYM) {
        return get_symbol(tokens[idx++].str);
    }
    return nullptr;
}

std::string s_input() {
    std::string s_text = "";
    int leftp = 0, rightp = 0;
    std::string line;
    do {
        std::cout << (s_text == "" ? "micro-scheme> " : "             > ");
        if (!std::getline(std::cin, line)) return "";
        for (char c : line) {
            if (c == '(') leftp++;
            else if (c == ')') rightp++;
        }
        s_text += line + " ";
    } while (leftp > rightp || s_text == " ");
    return s_text;
}

// ------------------------------------------------------------
// eval_from_source: 文字列から式を読み取り評価して結果を返す
//
// 手順:
//   1. tokenize で Token 列に変換
//   2. s_read で Scheme オブジェクトに変換
//   3. eval_expr で評価
//   評価中の GC でオブジェクトが回収されないよう expr と result を
//   ctx.s（スタック）に一時的に積んでルートとして保護する
// ------------------------------------------------------------
Object* eval_from_source(const std::string& source, VMContext& ctx, std::vector<Object*>& /* constants) */) {
    std::vector<Token> tokens = tokenize(source);
    int idx = 0;
    Object* expr = s_read(tokens, idx);
    ctx.s.push_back(expr);   // GC ルートとして保護
    Object* result = eval_expr(expr);
    ctx.s.push_back(result); // GC ルートとして保護
    return result;
}

std::vector<Object*> read_all_exprs_from_string(const std::string& text) {
    std::vector<Token> tokens = tokenize(text);
    std::vector<Object*> exprs;
    int idx = 0;
    while (idx < static_cast<int>(tokens.size()) && tokens[idx].type != TOK_END) {
        exprs.push_back(s_read(tokens, idx));
    }
    return exprs;
}

static std::string normalize_load_path(const std::string& raw_path) {
    if (raw_path.size() >= 2) {
        char first = raw_path.front();
        char last = raw_path.back();
        if ((first == '"' && last == '"') || (first == '\'' && last == '\'')) {
            return raw_path.substr(1, raw_path.size() - 2);
        }
    }
    return raw_path;
}

bool load_scheme_file(const std::string& path) {
    std::ifstream ifs(path);
    if (!ifs) {
        std::cerr << "[load] cannot open file: " << path << std::endl;
        return false;
    }

    std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    std::vector<Object*> exprs;
    try {
        exprs = read_all_exprs_from_string(content);
    } catch (const std::exception& e) {
        std::cerr << "[load] parse error in " << path << ": " << e.what() << std::endl;
        return false;
    }

    for (auto* expr : exprs) {
        try {
            eval_expr(expr);
        } catch (const std::exception& e) {
            std::cerr << "[load] eval error in " << path << ": " << e.what() << std::endl;
            return false;
        }
    }
    return true;
}

Object* prim_read_impl(std::vector<Object*>& args) {
    std::string text;
    if (!args.empty() && args[0] && args[0]->type == PORT && args[0]->port.file && args[0]->port.file->is_open()) {
        std::vector<std::string> lines;
        int depth = 0;
        bool has_content = false;
        std::string line;

        while (std::getline(*args[0]->port.file, line)) {
            std::string stripped;
            stripped.reserve(line.size());
            for (char ch : line) {
                if (ch == ';') break;
                stripped.push_back(ch);
            }
            while (!stripped.empty() && (stripped.back() == ' ' || stripped.back() == '\t' || stripped.back() == '\r')) {
                stripped.pop_back();
            }
            if (stripped.empty()) continue;

            for (char ch : stripped) {
                if (ch == '(') {
                    depth++;
                    has_content = true;
                } else if (ch == ')') {
                    depth--;
                } else if (ch != ' ' && ch != '\t') {
                    has_content = true;
                }
            }
            lines.push_back(stripped);
            if (has_content && depth <= 0) break;
        }

        if (lines.empty()) return get_symbol(":eof");

        for (size_t i = 0; i < lines.size(); ++i) {
            if (i > 0) text.push_back(' ');
            text += lines[i];
        }
    } else {
        if (!std::getline(std::cin, text)) return get_symbol(":eof");
    }

    if (text.empty()) return get_symbol(":eof");
    std::vector<Token> tokens = tokenize(text);
    int idx = 0;
    return s_read(tokens, idx);
}

void add_prim(const std::string& name, std::function<Object*(std::vector<Object*>&)> f) {
    Object* p = alloc(PRIMITIVE);
    p->prim = new std::function<Object*(std::vector<Object*>&)>(f);
    globals[name] = p;
}

// ------------------------------------------------------------
// プリミティブ関数の登録
//
// register_core_primitives() で以下の組み込み関数を globals に登録する:
//   リスト操作  : car, cdr, cons, set-car!, set-cdr!, list, pair?, null?
//   算術        : +, -, *, /, modulo, =, <, >, <=, >=
//   比較        : eq?, equal?, not
//   文字列      : symbol->string, number->string, string->number, string-append
//   表示        : display, newline
//   リスト高階  : length, append, reverse, map, for-each, filter,
//                 fold-left, fold-right, apply, assq, memq
//   I/O         : open-input-file, open-output-file, close-input-port,
//                 close-output-port, read-line, read-char, write, write-char,
//                 write_newline, eof-object?, read, read-expr, load
//   その他      : macroexpand-1, macroexpand, gensym, error
// ------------------------------------------------------------
void register_core_primitives() {
    add_prim("car", [](std::vector<Object*>& args) {
        if (args.size() > 0 && args[0] && args[0]->type == PAIR) return args[0]->pair.car;
        return (Object*)nullptr;
    });

    add_prim("cdr", [](std::vector<Object*>& args) {
        if (args.size() > 0 && args[0] && args[0]->type == PAIR) return args[0]->pair.cdr;
        return (Object*)nullptr;
    });

    add_prim("cons", [](std::vector<Object*>& args) {
        Object* p = alloc(PAIR);
        if (args.size() >= 2) {
            p->pair.car = args[0];
            p->pair.cdr = args[1];
        }
        return p;
    });

    add_prim("list", [](std::vector<Object*>& args) {
        Object* out = nullptr;
        for (auto it = args.rbegin(); it != args.rend(); ++it) {
            out = cons(*it, out);
        }
        return out;
    });

    add_prim("+", [](std::vector<Object*>& args) {
        long long res = 0;
        bool has_double = false;
        long double dres = 0.0L;
        
        for (auto o : args) {
            if (!o) continue;
            if (o->type == INT) {
                if (has_double) dres += (long double)o->num;
                else {
                    if (will_overflow_add((int)res, o->num)) {
                        has_double = true;
                        dres = (long double)res + (long double)o->num;
                    } else {
                        res += o->num;
                    }
                }
            } else if (o->type == INT64) {
                if (has_double) dres += (long double)o->num64;
                else {
                    // Check if res + o->num64 overflows long long (unlikely but possible)
                    if ((o->num64 > 0 && res > LLONG_MAX - o->num64) ||
                        (o->num64 < 0 && res < LLONG_MIN - o->num64)) {
                        has_double = true;
                        dres = (long double)res + (long double)o->num64;
                    } else {
                        res += o->num64;
                    }
                }
            } else if (o->type == DOUBLE) {
                has_double = true;
                if (dres == 0.0L) dres = (long double)res;
                dres += o->dbl;
            }
        }
        
        if (has_double) {
            return make_number_from_double(dres);
        } else {
            return make_number(res);
        }
    });

    add_prim("-", [](std::vector<Object*>& args) {
        long long res = 0;
        bool has_double = false;
        long double dres = 0.0L;
        
        if (!args.empty() && args[0]) {
            if (args[0]->type == INT) res = args[0]->num;
            else if (args[0]->type == INT64) res = args[0]->num64;
            else if (args[0]->type == DOUBLE) { has_double = true; dres = args[0]->dbl; }
        }
        
        for (size_t i = 1; i < args.size(); ++i) {
            if (!args[i]) continue;
            if (args[i]->type == INT) {
                if (has_double) dres -= (long double)args[i]->num;
                else {
                    if (will_overflow_sub((int)res, args[i]->num)) {
                        has_double = true;
                        dres = (long double)res - (long double)args[i]->num;
                    } else {
                        res -= args[i]->num;
                    }
                }
            } else if (args[i]->type == INT64) {
                if (has_double) dres -= (long double)args[i]->num64;
                else {
                    if ((args[i]->num64 < 0 && res > LLONG_MAX + args[i]->num64) ||
                        (args[i]->num64 > 0 && res < LLONG_MIN + args[i]->num64)) {
                        has_double = true;
                        dres = (long double)res - (long double)args[i]->num64;
                    } else {
                        res -= args[i]->num64;
                    }
                }
            } else if (args[i]->type == DOUBLE) {
                has_double = true;
                if (dres == 0.0L) dres = (long double)res;
                dres -= args[i]->dbl;
            }
        }
        
        if (has_double) {
            return make_number_from_double(dres);
        } else {
            return make_number(res);
        }
    });

    add_prim("*", [](std::vector<Object*>& args) {
        long long res = 1;
        bool has_double = false;
        long double dres = 1.0L;
        
        for (auto o : args) {
            if (!o) continue;
            if (o->type == INT) {
                if (has_double) dres *= (long double)o->num;
                else {
                    if (will_overflow_mul((int)res, o->num)) {
                        has_double = true;
                        dres = (long double)res * (long double)o->num;
                    } else {
                        res *= o->num;
                    }
                }
            } else if (o->type == INT64) {
                if (has_double) dres *= (long double)o->num64;
                else {
                    // Check overflow for long long multiplication
                    if (res != 0 && o->num64 != 0) {
                        if ((res > 0 && o->num64 > 0 && res > LLONG_MAX / o->num64) ||
                            (res < 0 && o->num64 < 0 && res < LLONG_MAX / o->num64) ||
                            (res > 0 && o->num64 < 0 && res > LLONG_MIN / o->num64) ||
                            (res < 0 && o->num64 > 0 && res < LLONG_MIN / o->num64)) {
                            has_double = true;
                            dres = (long double)res * (long double)o->num64;
                        } else {
                            res *= o->num64;
                        }
                    }
                }
            } else if (o->type == DOUBLE) {
                has_double = true;
                if (dres == 1.0L) dres = (long double)res;
                dres *= o->dbl;
            }
        }
        
        if (has_double) {
            return make_number_from_double(dres);
        } else {
            return make_number(res);
        }
    });

    add_prim("=", [](std::vector<Object*>& args) {
        if (args.size() < 2) return bool_obj(true);
        if (!args[0]) return bool_obj(false);
        
        // Get the first value
        long double base_val;
        if (args[0]->type == INT) base_val = (long double)args[0]->num;
        else if (args[0]->type == INT64) base_val = (long double)args[0]->num64;
        else if (args[0]->type == DOUBLE) base_val = args[0]->dbl;
        else return bool_obj(false);
        
        for (size_t i = 1; i < args.size(); ++i) {
            if (!args[i]) return bool_obj(false);
            long double val;
            if (args[i]->type == INT) val = (long double)args[i]->num;
            else if (args[i]->type == INT64) val = (long double)args[i]->num64;
            else if (args[i]->type == DOUBLE) val = args[i]->dbl;
            else return bool_obj(false);
            
            if (base_val != val) return bool_obj(false);
        }
        return bool_obj(true);
    });

    add_prim("<", [](std::vector<Object*>& args) {
        if (args.size() < 2) return bool_obj(true);
        for (size_t i = 1; i < args.size(); ++i) {
            if (!args[i - 1] || !args[i]) return bool_obj(false);
            
            long double prev, curr;
            if (args[i - 1]->type == INT) prev = (long double)args[i - 1]->num;
            else if (args[i - 1]->type == INT64) prev = (long double)args[i - 1]->num64;
            else if (args[i - 1]->type == DOUBLE) prev = args[i - 1]->dbl;
            else return bool_obj(false);
            
            if (args[i]->type == INT) curr = (long double)args[i]->num;
            else if (args[i]->type == INT64) curr = (long double)args[i]->num64;
            else if (args[i]->type == DOUBLE) curr = args[i]->dbl;
            else return bool_obj(false);
            
            if (!(prev < curr)) return bool_obj(false);
        }
        return bool_obj(true);
    });

    add_prim(">", [](std::vector<Object*>& args) {
        if (args.size() < 2) return bool_obj(true);
        for (size_t i = 1; i < args.size(); ++i) {
            if (!args[i - 1] || !args[i]) return bool_obj(false);
            
            long double prev, curr;
            if (args[i - 1]->type == INT) prev = (long double)args[i - 1]->num;
            else if (args[i - 1]->type == INT64) prev = (long double)args[i - 1]->num64;
            else if (args[i - 1]->type == DOUBLE) prev = args[i - 1]->dbl;
            else return bool_obj(false);
            
            if (args[i]->type == INT) curr = (long double)args[i]->num;
            else if (args[i]->type == INT64) curr = (long double)args[i]->num64;
            else if (args[i]->type == DOUBLE) curr = args[i]->dbl;
            else return bool_obj(false);
            
            if (!(prev > curr)) return bool_obj(false);
        }
        return bool_obj(true);
    });

    add_prim("<=", [](std::vector<Object*>& args) {
        if (args.size() < 2) return bool_obj(true);
        for (size_t i = 1; i < args.size(); ++i) {
            if (!args[i - 1] || !args[i]) return bool_obj(false);
            
            long double prev, curr;
            if (args[i - 1]->type == INT) prev = (long double)args[i - 1]->num;
            else if (args[i - 1]->type == INT64) prev = (long double)args[i - 1]->num64;
            else if (args[i - 1]->type == DOUBLE) prev = args[i - 1]->dbl;
            else return bool_obj(false);
            
            if (args[i]->type == INT) curr = (long double)args[i]->num;
            else if (args[i]->type == INT64) curr = (long double)args[i]->num64;
            else if (args[i]->type == DOUBLE) curr = args[i]->dbl;
            else return bool_obj(false);
            
            if (!(prev <= curr)) return bool_obj(false);
        }
        return bool_obj(true);
    });

    add_prim(">=", [](std::vector<Object*>& args) {
        if (args.size() < 2) return bool_obj(true);
        for (size_t i = 1; i < args.size(); ++i) {
            if (!args[i - 1] || !args[i]) return bool_obj(false);
            
            long double prev, curr;
            if (args[i - 1]->type == INT) prev = (long double)args[i - 1]->num;
            else if (args[i - 1]->type == INT64) prev = (long double)args[i - 1]->num64;
            else if (args[i - 1]->type == DOUBLE) prev = args[i - 1]->dbl;
            else return bool_obj(false);
            
            if (args[i]->type == INT) curr = (long double)args[i]->num;
            else if (args[i]->type == INT64) curr = (long double)args[i]->num64;
            else if (args[i]->type == DOUBLE) curr = args[i]->dbl;
            else return bool_obj(false);
            
            if (!(prev >= curr)) return bool_obj(false);
        }
        return bool_obj(true);
    });

    add_prim("null?", [](std::vector<Object*>& args) {
        return bool_obj(!args.empty() && args[0] == nullptr);
    });

    add_prim("pair?", [](std::vector<Object*>& args) {
        return bool_obj(!args.empty() && args[0] && args[0]->type == PAIR);
    });

    add_prim("not", [](std::vector<Object*>& args) {
        return bool_obj(!args.empty() && !is_true(args[0]));
    });

    add_prim("eq?", [](std::vector<Object*>& args) {
        return bool_obj(args.size() >= 2 && args[0] == args[1]);
    });

    add_prim("equal?", [](std::vector<Object*>& args) {
        return bool_obj(args.size() >= 2 && objects_equal(args[0], args[1]));
    });

    add_prim("append", [](std::vector<Object*>& args) {
        if (args.empty()) return (Object*)nullptr;
        if (args.size() == 1) return args[0];

        std::vector<Object*> out;
        for (size_t i = 0; i + 1 < args.size(); ++i) {
            Object* cur = args[i];
            while (cur) {
                if (cur->type != PAIR) return (Object*)nullptr;
                out.push_back(cur->pair.car);
                cur = cur->pair.cdr;
            }
        }

        Object* tail = args.back();
        Object* result = vector_to_pair(out);
        if (!result) return tail;
        Object* it = result;
        while (it && it->type == PAIR && it->pair.cdr) it = it->pair.cdr;
        if (it && it->type == PAIR) it->pair.cdr = tail;
        return result;
    });

    add_prim("display", [](std::vector<Object*>& args) {
        if (!args.empty()) print_obj(args[0]);
        return args.empty() ? (Object*)nullptr : args[0];
    });

    add_prim("newline", [](std::vector<Object*>& args) {
        (void)args;
        std::cout << std::endl;
        return (Object*)nullptr;
    });

    add_prim("print", [](std::vector<Object*>& args) {
        for (size_t i = 0; i < args.size(); ++i) {
            if (i > 0) std::cout << " ";
            print_obj(args[i]);
        }
        std::cout << std::endl;
        return args.empty() ? (Object*)nullptr : args.back();
    });

    add_prim("macroexpand-1", [](std::vector<Object*>& args) {
        if (args.empty()) return (Object*)nullptr;
        return macro_expand_1(args[0]);
    });

    add_prim("macroexpand", [](std::vector<Object*>& args) {
        if (args.empty()) return (Object*)nullptr;
        return macro_expand(args[0]);
    });

    // --- primitives missing from micro_scheme7 parity ---

    add_prim("/", [](std::vector<Object*>& args) {
        if (args.empty() || !args[0]) return (Object*)nullptr;
        
        long double result;
        if (args[0]->type == INT) result = (long double)args[0]->num;
        else if (args[0]->type == INT64) result = (long double)args[0]->num64;
        else if (args[0]->type == DOUBLE) result = args[0]->dbl;
        else return (Object*)nullptr;
        
        if (args.size() == 1) {
            if (result == 0.0L) return (Object*)nullptr;
            result = 1.0L / result;
        } else {
            for (size_t i = 1; i < args.size(); ++i) {
                if (!args[i]) return (Object*)nullptr;
                long double divisor;
                if (args[i]->type == INT) divisor = (long double)args[i]->num;
                else if (args[i]->type == INT64) divisor = (long double)args[i]->num64;
                else if (args[i]->type == DOUBLE) divisor = args[i]->dbl;
                else return (Object*)nullptr;
                
                if (divisor == 0.0L) return (Object*)nullptr;
                result /= divisor;
            }
        }
        return make_number_from_double(result);
    });

    add_prim("modulo", [](std::vector<Object*>& args) {
        if (args.size() != 2 || !args[0] || !args[1]) return (Object*)nullptr;
        
        long long a, b;
        if (args[0]->type == INT) a = (long long)args[0]->num;
        else if (args[0]->type == INT64) a = args[0]->num64;
        else return (Object*)nullptr;
        
        if (args[1]->type == INT) b = (long long)args[1]->num;
        else if (args[1]->type == INT64) b = args[1]->num64;
        else return (Object*)nullptr;
        
        if (b == 0) return (Object*)nullptr;
        return make_number(a % b);
    });

    add_prim("set-car!", [](std::vector<Object*>& args) {
        if (args.size() != 2 || !args[0] || args[0]->type != PAIR) return (Object*)nullptr;
        args[0]->pair.car = args[1];
        return args[1];
    });

    add_prim("set-cdr!", [](std::vector<Object*>& args) {
        if (args.size() != 2 || !args[0] || args[0]->type != PAIR) return (Object*)nullptr;
        args[0]->pair.cdr = args[1];
        return args[1];
    });

    add_prim("eqv?", [](std::vector<Object*>& args) {
        if (args.size() != 2) return bool_obj(false);
        if (args[0] == args[1]) return bool_obj(true);
        if (!args[0] || !args[1]) return bool_obj(false);
        if (args[0]->type == INT && args[1]->type == INT) return bool_obj(args[0]->num == args[1]->num);
        return bool_obj(false);
    });

    add_prim("symbol?", [](std::vector<Object*>& args) {
        return bool_obj(!args.empty() && args[0] && args[0]->type == SYMBOL);
    });

    add_prim("number?", [](std::vector<Object*>& args) {
        return bool_obj(!args.empty() && args[0] && 
                       (args[0]->type == INT || args[0]->type == INT64 || args[0]->type == DOUBLE));
    });

    add_prim("integer?", [](std::vector<Object*>& args) {
        return bool_obj(!args.empty() && args[0] && 
                       (args[0]->type == INT || args[0]->type == INT64));
    });

    add_prim("procedure?", [](std::vector<Object*>& args) {
        if (args.empty() || !args[0]) return bool_obj(false);
        return bool_obj(args[0]->type == PRIMITIVE || args[0]->type == CLOSURE || args[0]->type == CONTINUATION);
    });

    add_prim("reverse", [](std::vector<Object*>& args) {
        if (args.empty()) return (Object*)nullptr;
        std::vector<Object*> items = pair_to_vector(args[0]);
        std::reverse(items.begin(), items.end());
        return vector_to_pair(items);
    });

    add_prim("length", [](std::vector<Object*>& args) {
        if (args.empty()) return make_int(0);
        int len = 0;
        Object* cur = args[0];
        while (cur && cur->type == PAIR) { len++; cur = cur->pair.cdr; }
        return make_int(len);
    });

    add_prim("map", [](std::vector<Object*>& args) {
        if (args.size() < 2 || !args[0]) return (Object*)nullptr;
        std::vector<Object*> out;
        for (const auto& item : pair_to_vector(args[1])) {
            std::vector<Object*> one = {item};
            out.push_back(invoke_callable(args[0], one));
        }
        return vector_to_pair(out);
    });

    add_prim("filter", [](std::vector<Object*>& args) {
        if (args.size() < 2 || !args[0]) return (Object*)nullptr;
        std::vector<Object*> out;
        for (const auto& item : pair_to_vector(args[1])) {
            std::vector<Object*> one = {item};
            if (is_true(invoke_callable(args[0], one))) out.push_back(item);
        }
        return vector_to_pair(out);
    });

    add_prim("for-each", [](std::vector<Object*>& args) {
        if (args.size() < 2 || !args[0]) return (Object*)nullptr;
        for (const auto& item : pair_to_vector(args[1])) {
            std::vector<Object*> one = {item};
            invoke_callable(args[0], one);
        }
        return (Object*)nullptr;
    });

    add_prim("fold-left", [](std::vector<Object*>& args) {
        if (args.size() < 3 || !args[0]) return (Object*)nullptr;
        Object* result = args[1];
        for (const auto& item : pair_to_vector(args[2])) {
            std::vector<Object*> two = {result, item};
            result = invoke_callable(args[0], two);
        }
        return result;
    });

    add_prim("fold-right", [](std::vector<Object*>& args) {
        if (args.size() < 3 || !args[0]) return (Object*)nullptr;
        Object* result = args[1];
        std::vector<Object*> items = pair_to_vector(args[2]);
        for (auto it = items.rbegin(); it != items.rend(); ++it) {
            std::vector<Object*> two = {*it, result};
            result = invoke_callable(args[0], two);
        }
        return result;
    });

    add_prim("apply", [](std::vector<Object*>& args) {
        if (args.size() < 2 || !args[0]) return (Object*)nullptr;
        std::vector<Object*> prefix(args.begin() + 1, args.end() - 1);
        std::vector<Object*> tail = pair_to_vector(args.back());
        prefix.insert(prefix.end(), tail.begin(), tail.end());
        return invoke_callable(args[0], prefix);
    });

    add_prim("memq", [](std::vector<Object*>& args) {
        if (args.size() != 2) return bool_obj(false);
        Object* key = args[0];
        Object* cur = args[1];
        while (cur && cur->type == PAIR) {
            if (cur->pair.car == key) return cur;
            cur = cur->pair.cdr;
        }
        return bool_obj(false);
    });

    add_prim("assq", [](std::vector<Object*>& args) {
        if (args.size() != 2) return bool_obj(false);
        Object* key = args[0];
        Object* cur = args[1];
        while (cur && cur->type == PAIR) {
            Object* pair = cur->pair.car;
            if (pair && pair->type == PAIR && pair->pair.car == key) return pair;
            cur = cur->pair.cdr;
        }
        return bool_obj(false);
    });

    add_prim("list-tail", [](std::vector<Object*>& args) {
        if (args.size() != 2 || !args[1] || args[1]->type != INT) return (Object*)nullptr;
        int n = args[1]->num;
        Object* cur = args[0];
        while (n-- > 0 && cur && cur->type == PAIR) cur = cur->pair.cdr;
        return cur;
    });

    add_prim("list-ref", [](std::vector<Object*>& args) {
        if (args.size() != 2 || !args[1] || args[1]->type != INT) return (Object*)nullptr;
        int n = args[1]->num;
        Object* cur = args[0];
        while (n-- > 0 && cur && cur->type == PAIR) cur = cur->pair.cdr;
        return (cur && cur->type == PAIR) ? cur->pair.car : (Object*)nullptr;
    });

    int gensym_counter = 0;
    add_prim("gensym", [gensym_counter](std::vector<Object*>& args) mutable {
        std::string prefix = "g";
        if (!args.empty() && args[0] && args[0]->type == SYMBOL && args[0]->sym) prefix = *args[0]->sym;
        std::string name = prefix + std::to_string(++gensym_counter);
        return make_symbol_obj(name);
    });

    add_prim("number->string", [](std::vector<Object*>& args) {
        if (args.empty() || !args[0] || args[0]->type != INT) return (Object*)nullptr;
        return make_symbol_obj(std::to_string(args[0]->num));
    });

    add_prim("string->number", [](std::vector<Object*>& args) {
        if (args.empty() || !args[0] || args[0]->type != SYMBOL || !args[0]->sym) return bool_obj(false);
        try {
            int n = std::stoi(*args[0]->sym);
            return make_int(n);
        } catch (...) {
            return bool_obj(false);
        }
    });

    add_prim("string->symbol", [](std::vector<Object*>& args) {
        if (args.empty() || !args[0] || args[0]->type != SYMBOL || !args[0]->sym) return (Object*)nullptr;
        return get_symbol(*args[0]->sym);
    });

    add_prim("symbol->string", [](std::vector<Object*>& args) {
        if (args.empty() || !args[0] || args[0]->type != SYMBOL || !args[0]->sym) return (Object*)nullptr;
        return make_symbol_obj(*args[0]->sym);
    });

    add_prim("error", [](std::vector<Object*>& args) {
        std::string msg = "error";
        if (!args.empty() && args[0] && args[0]->type == SYMBOL && args[0]->sym) msg = *args[0]->sym;
        std::cerr << "[error] " << msg << std::endl;
        for (size_t i = 1; i < args.size(); ++i) {
            std::cerr << " ";
            print_obj(args[i]);
        }
        std::cerr << std::endl;
        return (Object*)nullptr;
    });

    add_prim("caar",  [](std::vector<Object*>& args) {
        if (args.empty() || !args[0] || args[0]->type != PAIR) return (Object*)nullptr;
        Object* a = args[0]->pair.car;
        return (a && a->type == PAIR) ? a->pair.car : (Object*)nullptr;
    });
    add_prim("cadr",  [](std::vector<Object*>& args) {
        if (args.empty() || !args[0] || args[0]->type != PAIR) return (Object*)nullptr;
        Object* d = args[0]->pair.cdr;
        return (d && d->type == PAIR) ? d->pair.car : (Object*)nullptr;
    });
    add_prim("cdar",  [](std::vector<Object*>& args) {
        if (args.empty() || !args[0] || args[0]->type != PAIR) return (Object*)nullptr;
        Object* a = args[0]->pair.car;
        return (a && a->type == PAIR) ? a->pair.cdr : (Object*)nullptr;
    });
    add_prim("cddr",  [](std::vector<Object*>& args) {
        if (args.empty() || !args[0] || args[0]->type != PAIR) return (Object*)nullptr;
        Object* d = args[0]->pair.cdr;
        return (d && d->type == PAIR) ? d->pair.cdr : (Object*)nullptr;
    });
    add_prim("caddr", [](std::vector<Object*>& args) {
        if (args.empty() || !args[0] || args[0]->type != PAIR) return (Object*)nullptr;
        Object* d1 = args[0]->pair.cdr;
        if (!d1 || d1->type != PAIR) return (Object*)nullptr;
        Object* d2 = d1->pair.cdr;
        return (d2 && d2->type == PAIR) ? d2->pair.car : (Object*)nullptr;
    });
    add_prim("cadddr",[](std::vector<Object*>& args) {
        if (args.empty() || !args[0] || args[0]->type != PAIR) return (Object*)nullptr;
        Object* cur = args[0]->pair.cdr;
        for (int i = 0; i < 2 && cur && cur->type == PAIR; ++i) cur = cur->pair.cdr;
        return (cur && cur->type == PAIR) ? cur->pair.car : (Object*)nullptr;
    });

    add_prim("open-input-file", [](std::vector<Object*>& args) {
        if (args.size() != 1 || !args[0] || args[0]->type != SYMBOL || !args[0]->sym) return (Object*)nullptr;
        Object* p = alloc(PORT);
        p->port.is_output = false;
        p->port.file = new std::fstream(*args[0]->sym, std::ios::in);
        if (!p->port.file->is_open()) {
            delete p->port.file;
            p->port.file = nullptr;
            return (Object*)nullptr;
        }
        return p;
    });

    add_prim("open-output-file", [](std::vector<Object*>& args) {
        if (args.size() != 1 || !args[0] || args[0]->type != SYMBOL || !args[0]->sym) return (Object*)nullptr;
        Object* p = alloc(PORT);
        p->port.is_output = true;
        p->port.file = new std::fstream(*args[0]->sym, std::ios::out | std::ios::trunc);
        if (!p->port.file->is_open()) {
            delete p->port.file;
            p->port.file = nullptr;
            return (Object*)nullptr;
        }
        return p;
    });

    auto prim_close_port = [](std::vector<Object*>& args) {
        if (args.size() != 1 || !args[0] || args[0]->type != PORT || !args[0]->port.file) return get_symbol(":undef");
        if (args[0]->port.file->is_open()) args[0]->port.file->close();
        return get_symbol(":undef");
    };
    add_prim("close-input-port", prim_close_port);
    add_prim("close-output-port", prim_close_port);

    add_prim("read-line", [](std::vector<Object*>& args) {
        if (args.size() != 1 || !args[0] || args[0]->type != PORT || !args[0]->port.file) return get_symbol(":eof");
        std::string line;
        if (!std::getline(*args[0]->port.file, line)) return get_symbol(":eof");
        return make_symbol_obj(line);
    });

    add_prim("read-char", [](std::vector<Object*>& args) {
        if (args.size() != 1 || !args[0] || args[0]->type != PORT || !args[0]->port.file) return get_symbol(":eof");
        int ch = args[0]->port.file->get();
        if (ch == EOF) return get_symbol(":eof");
        return make_symbol_obj(std::string(1, static_cast<char>(ch)));
    });

    add_prim("write", [](std::vector<Object*>& args) {
        if (args.size() != 2 || !args[1] || args[1]->type != PORT || !args[1]->port.file) return get_symbol(":undef");
        *args[1]->port.file << object_to_string(args[0]);
        return get_symbol(":undef");
    });

    add_prim("write_newline", [](std::vector<Object*>& args) {
        if (args.size() != 1 || !args[0] || args[0]->type != PORT || !args[0]->port.file) return get_symbol(":undef");
        *args[0]->port.file << '\n';
        return get_symbol(":undef");
    });

    add_prim("write-char", [](std::vector<Object*>& args) {
        if (args.size() != 2 || !args[1] || args[1]->type != PORT || !args[1]->port.file) return get_symbol(":undef");
        if (!args[0] || args[0]->type != SYMBOL || !args[0]->sym || args[0]->sym->empty()) return get_symbol(":undef");
        *args[1]->port.file << (*args[0]->sym)[0];
        return get_symbol(":undef");
    });

    add_prim("eof-object?", [](std::vector<Object*>& args) {
        if (args.size() != 1) return bool_obj(false);
        return bool_obj(is_symbol_name(args[0], ":eof"));
    });

    add_prim("read", [](std::vector<Object*>& args) {
        return prim_read_impl(args);
    });

    add_prim("read-expr", [](std::vector<Object*>& args) {
        return prim_read_impl(args);
    });

    add_prim("load", [](std::vector<Object*>& args) {
        if (args.size() != 1 || !args[0] || args[0]->type != SYMBOL || !args[0]->sym) return get_symbol(":undef");
        std::string path = normalize_load_path(*args[0]->sym);
        load_scheme_file(path);
        return get_symbol(":undef");
    });
}

// ------------------------------------------------------------
// 初期化: init_env
//
// 1. true / false / :undef / :eof 等の組み込み定数を globals に登録
// 2. register_core_primitives() でプリミティブ関数を登録
// 3. load_mlib=true のとき mlib7.scm または mlib8.scm を読み込んで
//    let, and, or, cond, map, fold 等の高レベル機能を定義する
// ------------------------------------------------------------
void init_env(bool load_mlib = true) {
    globals["true"] = alloc(INT); globals["true"]->num = 1;
    globals["false"] = alloc(INT); globals["false"]->num = 0;
    globals[":undef"] = get_symbol(":undef");
    globals[":eof"] = get_symbol(":eof");

    register_core_primitives();

    if (load_mlib) {
        VMContext bootstrap_ctx;
        std::vector<Object*> bootstrap_constants;
        VMContext* prev_ctx = g_active_ctx;
        std::vector<Object*>* prev_constants = g_active_constants;
        g_active_ctx = &bootstrap_ctx;
        g_active_constants = &bootstrap_constants;

        // try mlib7.scm first (same directory), fall back silently
        const char* candidates[] = {"mlib7.scm", "mlib8.scm", nullptr};
        for (int i = 0; candidates[i]; ++i) {
            std::ifstream probe(candidates[i]);
            if (probe.good()) {
                probe.close();
                load_scheme_file(candidates[i]);
                break;
            }
        }

        g_active_ctx = prev_ctx;
        g_active_constants = prev_constants;
    }
}

void cleanup_heap() {
    for (auto* obj : heap) {
        delete obj;
    }
    heap.clear();
    int_cache.clear();
    g_eval_env_stack.clear();
    g_closure_lexenv.clear();
}

// mlib7.scm 相当の総合テスト (scheme7 run_self_tests と同等)
int run_full_selftest() {
    init_env(true);
    VMContext test_ctx;
    std::vector<Object*> constants;
    g_active_ctx = &test_ctx;
    g_active_constants = &constants;

    int failures = 0;
    auto expect_eq = [&](const std::string& expr, const std::string& expected) {
        test_ctx.s.clear();
        Object* result = eval_from_source(expr, test_ctx, constants);
        std::string got;
        if (!result) got = "nil";
        else if (result->type == INT) got = (result->num ? (result->num == 1 && expected == "true" ? "true" : (result->num == 0 && expected == "false" ? "false" : std::to_string(result->num))) : "false");
        else got = object_to_string(result);
        // normalize bool display
        if (result && result->type == INT) {
            if (expected == "true" && result->num != 0) got = "true";
            else if (expected == "false" && result->num == 0) got = "false";
            else got = std::to_string(result->num);
        }
        if (got != expected) {
            failures++;
            std::cout << "[selftest-full][FAIL] " << expr
                      << "  expected=" << expected << "  got=" << got << std::endl;
        } else {
            std::cout << "[selftest-full][PASS] " << expr << std::endl;
        }
    };

    // --- basics ---
    expect_eq("(+ 1 2)", "3");
    expect_eq("(* 6 7)", "42");
    expect_eq("((lambda (a b) (+ a b)) 3 4)", "7");
    expect_eq("((lambda (x) (+ x 1)) 3)", "4");
    expect_eq("(if true 10 20)", "10");
    expect_eq("(if false 10 20)", "20");
    expect_eq("(quote a)", "a");
    expect_eq("(car (quote (a b c)))", "a");
    expect_eq("(cdr (quote (a b c)))", "(b c)");
    expect_eq("(cons (quote a) (quote b))", "(a . b)");
    expect_eq("(eq? (quote a) (quote a))", "true");
    expect_eq("(eq? (quote a) (quote b))", "false");
    expect_eq("(pair? (quote (a b c)))", "true");
    expect_eq("(pair? (quote a))", "false");
    expect_eq("(list (quote a) (quote b) (quote c) (quote d) (quote e))", "(a b c d e)");
    // --- define / set! ---
    expect_eq("(define x 9)", "x");
    expect_eq("x", "9");
    expect_eq("(set! x 7)", "7");
    expect_eq("x", "7");
    // --- call/cc ---
    expect_eq("(+ 100 (call/cc (lambda (k) (k 23))))", "123");
    expect_eq("(call/cc (lambda (k) 5))", "5");
    expect_eq("(+ 1 (call/cc (lambda (k) (k 41))))", "42");
    // --- varargs ---
    expect_eq("((lambda x x) 1 2 3)", "(1 2 3)");
    expect_eq("((lambda (a b . r) r) 1 2 3 4)", "(3 4)");
    expect_eq("((lambda (a b . r) (set! r (quote (9 8))) r) 1 2 3 4)", "(9 8)");
    // --- apply / arithmetic ---
    expect_eq("(apply + 1 (quote (2 3)))", "6");
    expect_eq("(/ 8 2)", "4");
    expect_eq("(modulo 7 3)", "1");
    // --- set-car!/set-cdr! ---
    expect_eq("(let ((p (cons (quote a) (quote b)))) (set-car! p (quote z)) p)", "(z . b)");
    expect_eq("(let ((p (cons (quote a) (quote b)))) (set-cdr! p (quote z)) p)", "(a . z)");
    // --- list ops ---
    expect_eq("(append (quote (a b c)) (quote (d e f)))", "(a b c d e f)");
    expect_eq("(append (quote ((a b) (c d))) (quote (e f g)))", "((a b) (c d) e f g)");
    expect_eq("(reverse (quote (a b c d e)))", "(e d c b a)");
    expect_eq("(reverse (quote ((a b) c (d e))))", "((d e) c (a b))");
    expect_eq("(memq (quote a) (quote (a b c d e)))", "(a b c d e)");
    expect_eq("(memq (quote c) (quote (a b c d e)))", "(c d e)");
    expect_eq("(memq (quote f) (quote (a b c d e)))", "false");
    expect_eq("(assq (quote a) (quote ((a 1) (b 2) (c 3) (d 4) (e 5))))", "(a 1)");
    expect_eq("(assq (quote e) (quote ((a 1) (b 2) (c 3) (d 4) (e 5))))", "(e 5)");
    expect_eq("(assq (quote f) (quote ((a 1) (b 2) (c 3) (d 4) (e 5))))", "false");
    // --- map / filter ---
    expect_eq("(map car (quote ((a 1) (b 2) (c 3) (d 4) (e 5))))", "(a b c d e)");
    expect_eq("(map cdr (quote ((a 1) (b 2) (c 3) (d 4) (e 5))))", "((1) (2) (3) (4) (5))");
    expect_eq("(map (lambda (x) (cons x x)) (quote (a b c d e)))", "((a . a) (b . b) (c . c) (d . d) (e . e))");
    expect_eq("(filter (lambda (x) (not (eq? x (quote a)))) (quote (a b c a b c a b c)))", "(b c b c b c)");
    // --- fold ---
    expect_eq("(fold-left cons (quote ()) (quote (a b c d e)))", "(((((nil . a) . b) . c) . d) . e)");
    expect_eq("(fold-right cons (quote ()) (quote (a b c d e)))", "(a b c d e)");
    // --- let / let* / letrec ---
    expect_eq("(let ((a 10) (b 20)) (cons a b))", "(10 . 20)");
    expect_eq("(let* ((a 10) (b 20) (c (cons a b))) c)", "(10 . 20)");
    expect_eq("(letrec ((a a)) a)", ":undef");
    expect_eq("(begin)", ":undef");
    expect_eq("(begin 1 2 3 4 5)", "5");
    // --- and / or ---
    expect_eq("(and 1 2 3)", "3");
    expect_eq("(and false 2 3)", "false");
    expect_eq("(and 1 false 3)", "false");
    expect_eq("(or 1 2 3)", "1");
    expect_eq("(or false 2 3)", "2");
    expect_eq("(or false false 3)", "3");
    expect_eq("(or false false false)", "false");
    // --- backquote ---
    expect_eq("(define a (quote (1 2 3)))", "a");
    expect_eq("`(a b c)", "(a b c)");
    expect_eq("`(,a b c)", "((1 2 3) b c)");
    expect_eq("`(,@a b c)", "(1 2 3 b c)");
    expect_eq("`(,(car a) b c)", "(1 b c)");
    expect_eq("`(,(cdr a) b c)", "((2 3) b c)");
    expect_eq("`(,@(cdr a) b c)", "(2 3 b c)");
    // --- define-macro / macroexpand ---
    expect_eq("(define-macro id (lambda (x) x))", "id");
    expect_eq("(macroexpand-1 (quote (id 42)))", "42");
    expect_eq("(macroexpand (quote (id (quote a))))", "(quote a)");
    expect_eq("(id (quote (1 2)))", "(1 2)");
    // --- cond / when / unless / case ---
    expect_eq("(cond ((= 1 2) 10) ((= 1 1) 42) (else 0))", "42");
    expect_eq("(when true 99)", "99");
    expect_eq("(unless false 88)", "88");
    expect_eq("(case 2 ((1) 10) ((2) 20) (else 30))", "20");
    // --- named let / do ---
    expect_eq("(let loop ((n 5) (acc 1)) (if (= n 0) acc (loop (- n 1) (* acc n))))", "120");
    expect_eq("(do ((i 0 (+ i 1)) (s 0 (+ s i))) ((= i 5) s))", "10");

    if (failures == 0) {
        std::cout << "[selftest-full] all tests passed." << std::endl;
    } else {
        std::cout << "[selftest-full] " << failures << " test(s) FAILED." << std::endl;
    }

    cleanup_heap();
    globals.clear();
    macros.clear();
    symbols.clear();
    g_active_ctx = nullptr;
    g_active_constants = nullptr;
    return failures == 0 ? 0 : 1;
}

int run_gc_selftest() {
    init_env(false);
    VMContext test_ctx;
    std::vector<Object*> constants;

    g_active_ctx = &test_ctx;
    g_active_constants = &constants;

    for (int i = 0; i < 200; ++i) {
        Object* n = make_int(i);
        test_ctx.s.push_back(n);
        if (test_ctx.s.size() > 3) {
            test_ctx.s.erase(test_ctx.s.begin());
        }
    }

    gc(test_ctx, constants);
    std::cout << "[selftest] heap size after gc: " << heap.size() << std::endl;
    cleanup_heap();
    globals.clear();
    macros.clear();
    symbols.clear();
    g_active_ctx = nullptr;
    g_active_constants = nullptr;
    return 0;
}

int run_eval_selftest() {
    init_env(false);
    VMContext test_ctx;
    std::vector<Object*> constants;
    g_active_ctx = &test_ctx;
    g_active_constants = &constants;
    g_vm_subset_success_count = 0;

    struct EvalCase {
        std::string expr;
        int expected_int;
    };

    const std::vector<EvalCase> cases = {
        {"(begin (define x 1) (set! x (+ x 2)) x)", 3},
        {"(let ((x 10)) x)", 10},
        {"(let* ((x 2) (y (+ x 3))) y)", 5},
        {"(letrec ((fact (lambda (n) (if (= n 0) 1 (* n (fact (- n 1))))))) (fact 5))", 120},
        {"(letrec ((even? (lambda (n) (if (= n 0) 1 (odd? (- n 1))))) (odd? (lambda (n) (if (= n 0) 0 (even? (- n 1)))))) (even? 6))", 1},
        {"(begin (define (inc x) (+ x 1)) (inc 41))", 42},
        {"(begin (define-macro (id x) x) (id 8))", 8},
        {"(begin (define-macro (id x) x) (macroexpand-1 '(id 9)))", 9},
        {"(begin (define-macro (id x) x) (macroexpand '(id (id 3))))", 3},
        {"(begin (define-macro (id x) x) (id 7))", 7},
        {"(begin (define q `(1 ,(+ 1 2) 4)) (car (cdr q)))", 3},
        {"(call/cc (lambda (k) (k 9) 5))", 9},
        // new parity cases
        {"(/ 10 2)", 5},
        {"(modulo 10 3)", 1},
        {"(begin (define p (cons 1 2)) (set-car! p 7) (car p))", 7},
        {"(begin (define p (cons 1 2)) (set-cdr! p 8) (cdr p))", 8},
        {"(length (list 1 2 3))", 3},
        {"(car (reverse (list 1 2 3)))", 3},
        {"(cadr (list 10 20 30))", 20},
        {"(begin (define (sq x) (* x x)) (car (map sq (list 2 3))))", 4},
        {"(begin (define r 0) (for-each (lambda (x) (set! r (+ r x))) (list 1 2 3)) r)", 6},
        {"(apply + (list 1 2 3))", 6},
        {"(fold-left + 0 (list 1 2 3 4))", 10},
        {"(fold-right - 0 (list 1))", 1},
        // and / or
        {"(and 1 2 3)", 3},
        {"(and 1 false 3)", 0},
        {"(or false false 5)", 5},
        {"(or false false false)", 0},
        // cond
        {"(cond ((= 1 2) 10) ((= 1 1) 42) (else 0))", 42},
        {"(cond (else 7))", 7},
        // when / unless
        {"(begin (define r 0) (when true (set! r 1)) r)", 1},
        {"(begin (define r 0) (unless false (set! r 2)) r)", 2},
        // case
        {"(case 2 ((1) 10) ((2) 20) (else 30))", 20},
        // named let
        {"(let loop ((n 5) (acc 1)) (if (= n 0) acc (loop (- n 1) (* acc n))))", 120},
        // do
        {"(do ((i 0 (+ i 1)) (s 0 (+ s i))) ((= i 5) s))", 10},
        {"(letrec ((fact (lambda (n a) (if (= n 0) a (fact (- n 1) (* a n)))))) (> (fact 40 1) 0))", 1}
    };

    int failures = 0;
    int vm_used_cases = 0;
    for (const auto& tc : cases) {
        test_ctx.s.clear();
        int before_vm = g_vm_subset_success_count;
        Object* result = eval_from_source(tc.expr, test_ctx, constants);
        bool used_vm = g_vm_subset_success_count > before_vm;
        if (used_vm) vm_used_cases++;
        std::cout << "[selftest-eval][ROUTE] " << (used_vm ? "vm" : "evaluator") << " expr=" << tc.expr << std::endl;
        if (!result || result->type != INT || result->num != tc.expected_int) {
            failures++;
            std::cout << "[selftest-eval][FAIL] expr=" << tc.expr << " expected=" << tc.expected_int << " got=";
            if (result && result->type == INT) std::cout << result->num;
            else print_obj(result);
            std::cout << std::endl;
        }
    }

    if (failures == 0) {
        std::cout << "[selftest-eval] all tests passed (" << cases.size() << ")" << std::endl;
    }

    if (g_vm_subset_success_count == 0) {
        failures++;
        std::cout << "[selftest-eval][FAIL] vm subset route was not used" << std::endl;
    }

    std::cout << "[selftest-eval] vm route used in " << vm_used_cases << "/" << cases.size() << " cases" << std::endl;

    cleanup_heap();
    globals.clear();
    macros.clear();
    symbols.clear();
    g_active_ctx = nullptr;
    g_active_constants = nullptr;
    return failures == 0 ? 0 : 1;
}

// ------------------------------------------------------------
// main: エントリポイント
//
// 起動オプション:
//   --selftest       : GC の簡易セルフテストを実行
//   --selftest-eval  : 評価器・コンパイラの動作テストを実行
//   --selftest-full  : mlib を含む総合テストを実行
//   (引数なし)       : REPL を起動
//
// REPL ループ:
//   1. s_input() で入力を読む（括弧が閉じるまで複数行対応）
//   2. eval_from_source() で評価（内部でコンパイラ→VM が動く）
//   3. 結果と Heap size を表示
//   4. "exit" または EOF で終了
// ------------------------------------------------------------
int main(int argc, char** argv) {
    if (argc > 1 && std::string(argv[1]) == "--selftest") {
        return run_gc_selftest();
    }
    if (argc > 1 && std::string(argv[1]) == "--selftest-eval") {
        return run_eval_selftest();
    }
    if (argc > 1 && std::string(argv[1]) == "--selftest-full") {
        return run_full_selftest();
    }

    init_env();

    VMContext repl_ctx;
    std::vector<Object*> constants; 

    g_active_ctx = &repl_ctx;
    g_active_constants = &constants;

    while (true) {
        std::string input = s_input();
        if (input == "" || input.find("exit") != std::string::npos) break;

        try {
            std::vector<Token> tokens = tokenize(input);
            int idx = 0;
            Object* expr = s_read(tokens, idx);

            repl_ctx.s.push_back(expr);
            Object* result = eval_from_source(input, repl_ctx, constants);
            repl_ctx.s.push_back(result);

            print_obj(result);
            std::cout << std::endl;
            repl_ctx.s.clear();

            std::cout << "\nHeap size: " << heap.size() << "\n\n";
        } catch (const ParseError& e) {
            std::cout << "Parse Error: " << e.what() << std::endl;
        } catch (const VMError& e) {
            std::cout << "VM Error: " << e.what() << std::endl;
        } catch (const std::exception& e) {
            std::cout << "Error: " << e.what() << std::endl;
        }
    }

    cleanup_heap();
    globals.clear();
    macros.clear();
    symbols.clear();
    int_cache.clear();
}


