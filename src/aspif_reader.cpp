#include "ddasp/aspif_reader.h"

#include <cstdlib>
#include <fstream>
#include <sstream>

namespace ddasp {

namespace {

/** Tokenizer over one aspif line, tracking the position for output strings. */
class LineTokens {
public:
    LineTokens(const std::string& line, int line_no) : line_(line), pos_(0), line_no_(line_no) {}

    bool at_end() {
        skip_spaces();
        return pos_ >= line_.size();
    }

    long long next_int(const char* what) {
        skip_spaces();
        if (pos_ >= line_.size()) throw AspifError(std::string("expected ") + what, line_no_);
        size_t start = pos_;
        if (line_[pos_] == '-' || line_[pos_] == '+') ++pos_;
        if (pos_ >= line_.size() || !isdigit(static_cast<unsigned char>(line_[pos_]))) {
            throw AspifError(std::string("expected integer for ") + what, line_no_);
        }
        while (pos_ < line_.size() && isdigit(static_cast<unsigned char>(line_[pos_]))) ++pos_;
        return std::strtoll(line_.substr(start, pos_ - start).c_str(), 0, 10);
    }

    int next_int32(const char* what) {
        long long v = next_int(what);
        if (v > 0x7fffffffLL || v < -0x7fffffffLL) {
            throw AspifError(std::string("integer out of range for ") + what, line_no_);
        }
        return static_cast<int>(v);
    }

    size_t next_count(const char* what) {
        long long v = next_int(what);
        if (v < 0) throw AspifError(std::string("negative count for ") + what, line_no_);
        return static_cast<size_t>(v);
    }

    /** Read exactly `len` bytes after a single separating space. */
    std::string next_fixed_string(size_t len) {
        if (pos_ < line_.size() && line_[pos_] == ' ') ++pos_;
        if (pos_ + len > line_.size()) throw AspifError("output string shorter than declared length", line_no_);
        std::string s = line_.substr(pos_, len);
        pos_ += len;
        return s;
    }

    void expect_end() {
        if (!at_end()) throw AspifError("unexpected trailing tokens", line_no_);
    }

private:
    void skip_spaces() {
        while (pos_ < line_.size() && (line_[pos_] == ' ' || line_[pos_] == '\t' || line_[pos_] == '\r')) ++pos_;
    }

    const std::string& line_;
    size_t pos_;
    int line_no_;
};

Atom read_atom(LineTokens& t, int line_no, const char* what) {
    int a = t.next_int32(what);
    if (a <= 0) throw AspifError(std::string("atom must be positive in ") + what, line_no);
    return a;
}

Literal read_literal(LineTokens& t, int line_no, const char* what) {
    int l = t.next_int32(what);
    if (l == 0) throw AspifError(std::string("literal must be nonzero in ") + what, line_no);
    return l;
}

std::vector<Literal> read_literals(LineTokens& t, int line_no, const char* what) {
    size_t n = t.next_count(what);
    std::vector<Literal> lits;
    lits.reserve(n);
    for (size_t i = 0; i < n; ++i) lits.push_back(read_literal(t, line_no, what));
    return lits;
}

std::vector<WeightedLiteral> read_weighted_literals(LineTokens& t, int line_no, const char* what) {
    size_t n = t.next_count(what);
    std::vector<WeightedLiteral> lits;
    lits.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        Literal l = read_literal(t, line_no, what);
        long long w = t.next_int(what);
        lits.push_back(WeightedLiteral(l, w));
    }
    return lits;
}

void read_rule(LineTokens& t, int line_no, Program& prog) {
    int head_kind = t.next_int32("head type");
    if (head_kind != 0 && head_kind != 1) throw AspifError("unknown head type", line_no);
    size_t m = t.next_count("head size");
    std::vector<Atom> head;
    head.reserve(m);
    for (size_t i = 0; i < m; ++i) head.push_back(read_atom(t, line_no, "head"));

    int body_kind = t.next_int32("body type");
    Body body;
    if (body_kind == 0) {
        body = Body::normal(read_literals(t, line_no, "body"));
    } else if (body_kind == 1) {
        long long bound = t.next_int("lower bound");
        body = Body::weight(bound, read_weighted_literals(t, line_no, "weight body"));
    } else {
        throw AspifError("unknown body type", line_no);
    }
    t.expect_end();
    prog.add_rule(Rule(head_kind == 1 ? HeadType::Choice : HeadType::Disjunction, head, body));
}

void read_minimize(LineTokens& t, int line_no, Program& prog) {
    int priority = t.next_int32("priority");
    std::vector<WeightedLiteral> lits = read_weighted_literals(t, line_no, "minimize");
    t.expect_end();
    prog.add_minimize(Minimize(priority, lits));
}

void read_projection(LineTokens& t, int line_no, Program& prog) {
    size_t n = t.next_count("projection size");
    std::vector<Atom> atoms;
    for (size_t i = 0; i < n; ++i) atoms.push_back(read_atom(t, line_no, "projection"));
    t.expect_end();
    prog.add_projection(atoms);
}

void read_output(LineTokens& t, int line_no, Program& prog) {
    size_t len = t.next_count("output string length");
    std::string name = t.next_fixed_string(len);
    std::vector<Literal> cond = read_literals(t, line_no, "output condition");
    t.expect_end();
    prog.add_output(Output(name, cond));
}

void read_external(LineTokens& t, int line_no, Program& prog) {
    Atom a = read_atom(t, line_no, "external");
    int v = t.next_int32("external value");
    if (v < 0 || v > 3) throw AspifError("unknown external value", line_no);
    t.expect_end();
    prog.add_external(External(a, static_cast<ExternalValue>(v)));
}

void warn(std::vector<std::string>* warnings, int line_no, const std::string& msg) {
    if (warnings) warnings->push_back("aspif line " + std::to_string(line_no) + ": " + msg);
}

}  // namespace

Program read_aspif(std::istream& in, std::vector<std::string>* warnings) {
    Program prog;
    std::string line;
    int line_no = 0;
    bool header_seen = false;

    while (std::getline(in, line)) {
        ++line_no;
        LineTokens t(line, line_no);
        if (t.at_end()) continue;

        if (!header_seen) {
            // "asp 1 0 0 [incremental]"
            std::istringstream hs(line);
            std::string word;
            hs >> word;
            if (word != "asp") throw AspifError("missing 'asp' header", line_no);
            int major = 0, minor = 0, revision = 0;
            if (!(hs >> major >> minor >> revision)) throw AspifError("malformed header", line_no);
            if (major != 1) throw AspifError("unsupported aspif major version", line_no);
            header_seen = true;
            continue;
        }

        int type = t.next_int32("statement type");
        switch (type) {
            case 0:
                t.expect_end();
                break;  // end of (one step of) the program; keep reading for incremental input
            case 1:
                read_rule(t, line_no, prog);
                break;
            case 2:
                read_minimize(t, line_no, prog);
                break;
            case 3:
                read_projection(t, line_no, prog);
                break;
            case 4:
                read_output(t, line_no, prog);
                break;
            case 5:
                read_external(t, line_no, prog);
                break;
            case 6:
                warn(warnings, line_no, "assumption statement ignored");
                break;
            case 7:
                warn(warnings, line_no, "heuristic statement ignored");
                break;
            case 8:
                warn(warnings, line_no, "edge statement ignored");
                break;
            case 9:
                warn(warnings, line_no, "theory statement ignored");
                break;
            case 10:
                break;  // comment
            default:
                throw AspifError("unknown statement type " + std::to_string(type), line_no);
        }
    }
    if (!header_seen) throw AspifError("empty input (no aspif header)", line_no);
    return prog;
}

Program read_aspif_string(const std::string& text, std::vector<std::string>* warnings) {
    std::istringstream in(text);
    return read_aspif(in, warnings);
}

Program read_aspif_file(const std::string& path, std::vector<std::string>* warnings) {
    std::ifstream in(path.c_str());
    if (!in) throw std::runtime_error("cannot open aspif file: " + path);
    return read_aspif(in, warnings);
}

}  // namespace ddasp
