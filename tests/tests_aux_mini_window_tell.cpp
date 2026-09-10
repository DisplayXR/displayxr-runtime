// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Pin the OEM container-scaled tell across its implementations (#1401).
 *
 * The tell — "this window reports a logical extent at a physical origin, so it
 * spills off the panel" — decides whether an Android window weaves at all. After
 * #1398 it existed in three places:
 *
 *   - `comp_vk_native_compositor.c` (weave, or degrade to 2D),
 *   - `oxr_android_surface.c`       (hint, or do not),
 *   - `MiniWindowLayout.java`       (answer BEFORE any C sees the rect).
 *
 * #1401 collapses the two C copies into @ref android_mini_window_is_tell. The
 * Java one cannot be collapsed — it runs in the aux AAR, ahead of the C — so it
 * is pinned HERE instead: the test parses `isTell`'s expression straight out of
 * the Java source and evaluates it against the same case table as the C.
 *
 * For `isTell` that is a SEMANTIC comparison, not a string one: the expression is
 * parsed and evaluated, so a reformat (the project runs google-java-format over
 * that file) must not fail this test while a quietly ADDED SLACK — `x < -8`,
 * `w > dispW * 1.02` — must.
 *
 * `isBindingTell` and its mid-rotation reject are pinned only by SHAPE — a
 * substring match on the guard and on the delegation to `isTell`. They have no C
 * counterpart to compare against and the reject is a statement rather than an
 * expression, so the evaluator cannot reach them. That is a weaker pin and is
 * called out here so nobody reads the file as proving more than it does;
 * extending the evaluator to cover them is tracked as a follow-up.
 *
 * No Android, no JNI, no device: the rule is integer arithmetic, and the whole
 * point of extracting it is that it is now findable on the host.
 */

#include "catch_amalgamated.hpp"

#include "android/android_mini_window_tell.h"

#include <cctype>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

/*
 *
 * The case table. One row per behaviour worth naming.
 *
 */

struct Case
{
	const char *name;
	int32_t x, y;
	uint32_t w, h;
	uint32_t disp_w, disp_h;
	bool expect;
};

// clang-format off
const Case cases[] = {
    // The measured NP02J mini-window: a 1080x1685 logical window placed at the
    // physical origin of a 0.67-scaled container on a 2560x1600 panel.
    {"np02j mini-window landscape",   1757,  236, 1080, 1685, 2560, 1600, true},
    // The same task in portrait (measured origin 797,716 after the rotation leg).
    {"np02j mini-window portrait",     797,  716, 1685, 1080, 1600, 2560, true},
    // Fullscreen: exactly the panel, at the origin. The single most important
    // false — an ungated vendor probe here would shrink a fullscreen buffer.
    {"fullscreen landscape",              0,    0, 2560, 1600, 2560, 1600, false},
    {"fullscreen portrait",               0,    0, 1600, 2560, 1600, 2560, false},
    // The window the app publishes ONCE IT HAS APPLIED the hint: physical, so it
    // fits, so the tell is false and the latch (not the tell) keeps the episode.
    {"applied hint, physical rect",    1757,  236,  723, 1129, 2560, 1600, false},
    // Split-screen: genuinely smaller window, genuinely fits. Never a tell.
    {"split screen top half",             0,    0, 2560,  800, 2560, 1600, false},
    // Touching the far edge exactly is NOT spilling. Off by one either way is
    // the classic place for a copy to drift.
    {"flush against the right edge",   1480,    0, 1080, 1600, 2560, 1600, false},
    {"one px past the right edge",     1481,    0, 1080, 1600, 2560, 1600, true},
    {"flush against the bottom edge",     0, 1000, 1080,  600, 2560, 1600, false},
    {"one px past the bottom edge",       0, 1001, 1080,  600, 2560, 1600, true},
    // A negative origin is a spill on its own — a window dragged off the left or
    // top edge. Costs 2D content, never a broken weave.
    {"dragged off the left edge",       -40,  100,  800,  600, 2560, 1600, true},
    {"dragged off the top edge",        100,  -40,  800,  600, 2560, 1600, true},
    // ONE pixel off. The tell has no slack by design (it mirrors the browser's
    // heuristic exactly), and a "harmless" tolerance added to one copy is the
    // drift this table is here to catch, so it has to be tripped by a spill too
    // small for a generous case to notice.
    {"one px off the left edge",         -1,  100,  800,  600, 2560, 1600, true},
    {"one px off the top edge",         100,   -1,  800,  600, 2560, 1600, true},
    // Degenerate inputs. Every caller's rule is "never decide on ignorance", so
    // a missing panel extent or a zero window answers false and the caller
    // checks for it separately.
    // ZERO EXTENT AT A SPILLING ORIGIN. Without these, dropping `w > 0 && h > 0`
    // from the Java SURVIVES: the only zero-extent row sat at 1757,236, where no
    // spill term fires anyway, so the guard was never load-bearing (#1401 review).
    {"zero extent, spilling x origin", 2600,    0,    0,    0, 2560, 1600, false},
    {"zero extent, spilling y origin",    0, 1700,    0,    0, 2560, 1600, false},
    // PANEL HEIGHT MISSING ON ITS OWN. Without this, dropping the `disp_h == 0`
    // term from the C header SURVIVES: every other row sets both panel axes or
    // neither.
    {"panel height missing only",         0,    0, 1080, 1685, 2560,    0, false},
    {"no panel extent yet",            1757,  236, 1080, 1685,    0,    0, false},
    {"no window extent yet",           1757,  236,    0,    0, 2560, 1600, false},
    {"zero panel width only",             0,    0, 1080, 1685,    0, 1600, false},
};
// clang-format on


/*
 *
 * A very small evaluator for the ONE Java expression this file pins.
 *
 * Grammar (all that `isTell` uses, and all that it is allowed to grow into
 * without someone consciously extending this):
 *
 *   or   := and { "||" and }
 *   and  := cmp { "&&" cmp }
 *   cmp  := add [ ("<" | ">" | "<=" | ">=" | "==" | "!=") add ]
 *   add  := mul { ("+" | "-") mul }
 *   mul  := unary { ("*" | "/") unary }
 *   unary:= [ "-" | "!" ] primary
 *   prim := IDENT | INT | "(" or ")"
 *
 * Evaluated in int64, which is what the C helper promotes to. Java would wrap at
 * 2^31; no panel or window coordinate is within nine orders of magnitude of
 * that, so the two agree on everything reachable, and the test says so rather
 * than pretending the languages are identical.
 *
 */

struct Env
{
	int64_t x, y, w, h, dispW, dispH;
};

class Parser
{
public:
	Parser(const std::vector<std::string> &toks, const Env &env) : toks_(toks), env_(env) {}

	int64_t
	parse()
	{
		int64_t v = parseOr();
		if (pos_ != toks_.size()) {
			throw std::runtime_error("trailing tokens in the Java expression at '" + toks_[pos_] + "'");
		}
		return v;
	}

private:
	const std::vector<std::string> &toks_;
	const Env &env_;
	size_t pos_ = 0;

	bool
	accept(const char *t)
	{
		if (pos_ < toks_.size() && toks_[pos_] == t) {
			pos_++;
			return true;
		}
		return false;
	}

	int64_t
	parseOr()
	{
		int64_t v = parseAnd();
		while (accept("||")) {
			// No short-circuit: both sides are pure comparisons on ints.
			int64_t r = parseAnd();
			v = (v != 0 || r != 0) ? 1 : 0;
		}
		return v;
	}

	int64_t
	parseAnd()
	{
		int64_t v = parseCmp();
		while (accept("&&")) {
			int64_t r = parseCmp();
			v = (v != 0 && r != 0) ? 1 : 0;
		}
		return v;
	}

	int64_t
	parseCmp()
	{
		int64_t v = parseAdd();
		if (pos_ >= toks_.size()) {
			return v;
		}
		const std::string &op = toks_[pos_];
		if (op == "<" || op == ">" || op == "<=" || op == ">=" || op == "==" || op == "!=") {
			pos_++;
			int64_t r = parseAdd();
			if (op == "<") {
				return v < r ? 1 : 0;
			}
			if (op == ">") {
				return v > r ? 1 : 0;
			}
			if (op == "<=") {
				return v <= r ? 1 : 0;
			}
			if (op == ">=") {
				return v >= r ? 1 : 0;
			}
			if (op == "==") {
				return v == r ? 1 : 0;
			}
			return v != r ? 1 : 0;
		}
		return v;
	}

	int64_t
	parseAdd()
	{
		int64_t v = parseMul();
		for (;;) {
			if (accept("+")) {
				v += parseMul();
			} else if (accept("-")) {
				v -= parseMul();
			} else {
				return v;
			}
		}
	}

	int64_t
	parseMul()
	{
		int64_t v = parseUnary();
		for (;;) {
			if (accept("*")) {
				v *= parseUnary();
			} else if (accept("/")) {
				int64_t r = parseUnary();
				if (r == 0) {
					throw std::runtime_error("division by zero in the Java expression");
				}
				v /= r;
			} else {
				return v;
			}
		}
	}

	int64_t
	parseUnary()
	{
		if (accept("-")) {
			return -parseUnary();
		}
		if (accept("!")) {
			return parseUnary() == 0 ? 1 : 0;
		}
		return parsePrimary();
	}

	int64_t
	parsePrimary()
	{
		if (pos_ >= toks_.size()) {
			throw std::runtime_error("unexpected end of the Java expression");
		}
		if (accept("(")) {
			int64_t v = parseOr();
			if (!accept(")")) {
				throw std::runtime_error("unbalanced parentheses in the Java expression");
			}
			return v;
		}
		const std::string t = toks_[pos_++];
		if (std::isdigit(static_cast<unsigned char>(t[0])) != 0) {
			return std::stoll(t);
		}
		if (t == "x") {
			return env_.x;
		}
		if (t == "y") {
			return env_.y;
		}
		if (t == "w") {
			return env_.w;
		}
		if (t == "h") {
			return env_.h;
		}
		if (t == "dispW") {
			return env_.dispW;
		}
		if (t == "dispH") {
			return env_.dispH;
		}
		if (t == "true") {
			return 1;
		}
		if (t == "false") {
			return 0;
		}
		// Anything else is a NEW input the tell has grown, which is exactly the
		// drift this test exists to catch — fail loudly rather than guess.
		throw std::runtime_error("unknown identifier '" + t + "' in the Java expression");
	}
};

std::vector<std::string>
tokenize(const std::string &src)
{
	static const char *twoChar[] = {"&&", "||", "<=", ">=", "==", "!="};
	std::vector<std::string> out;
	size_t i = 0;
	while (i < src.size()) {
		const char c = src[i];
		if (std::isspace(static_cast<unsigned char>(c)) != 0) {
			i++;
			continue;
		}
		bool matched = false;
		for (const char *t : twoChar) {
			if (src.compare(i, 2, t) == 0) {
				out.emplace_back(t);
				i += 2;
				matched = true;
				break;
			}
		}
		if (matched) {
			continue;
		}
		if (std::isalpha(static_cast<unsigned char>(c)) != 0 || c == '_') {
			size_t j = i;
			while (j < src.size() &&
			       (std::isalnum(static_cast<unsigned char>(src[j])) != 0 || src[j] == '_')) {
				j++;
			}
			out.push_back(src.substr(i, j - i));
			i = j;
			continue;
		}
		if (std::isdigit(static_cast<unsigned char>(c)) != 0) {
			size_t j = i;
			while (j < src.size() && std::isdigit(static_cast<unsigned char>(src[j])) != 0) {
				j++;
			}
			out.push_back(src.substr(i, j - i));
			i = j;
			continue;
		}
		out.emplace_back(1, c);
		i++;
	}
	return out;
}

std::string
readFile(const char *path)
{
	std::ifstream f(path);
	if (!f) {
		throw std::runtime_error(std::string("cannot open ") + path);
	}
	std::ostringstream ss;
	ss << f.rdbuf();
	return ss.str();
}

/*!
 * Blank out every comment, keeping the source's length and line structure.
 *
 * MANDATORY before any of the parsing below, and the reason is this file's own
 * history: the class javadoc documents the cross-APK contract by NAME, so it
 * contains the literal text `{@code boolean isTell(int, int, ...)}`. A plain
 * `find("boolean isTell(")` matched THAT, ran the brace matcher over prose, and
 * the whole test died with "no return statement" — after passing locally, on a
 * binary built before the javadoc existed.
 *
 * Replacing with spaces rather than erasing keeps every later offset meaningful,
 * so a failure still points at the right place in the real file.
 */
std::string
stripComments(const std::string &src)
{
	std::string out = src;
	enum
	{
		CODE,
		LINE_COMMENT,
		BLOCK_COMMENT,
		STRING_LIT,
		CHAR_LIT
	} st = CODE;

	for (size_t i = 0; i < out.size(); i++) {
		const char c = out[i];
		const char n = (i + 1 < out.size()) ? out[i + 1] : '\0';
		switch (st) {
		case CODE:
			if (c == '/' && n == '/') {
				st = LINE_COMMENT;
				out[i] = out[i + 1] = ' ';
				i++;
			} else if (c == '/' && n == '*') {
				st = BLOCK_COMMENT;
				out[i] = out[i + 1] = ' ';
				i++;
			} else if (c == '"') {
				st = STRING_LIT;
				out[i] = ' ';
			} else if (c == '\'') {
				st = CHAR_LIT;
				out[i] = ' ';
			}
			break;
		case LINE_COMMENT:
			if (c == '\n') {
				st = CODE;
			} else {
				out[i] = ' ';
			}
			break;
		case BLOCK_COMMENT:
			if (c == '*' && n == '/') {
				out[i] = out[i + 1] = ' ';
				i++;
				st = CODE;
			} else if (c != '\n') {
				out[i] = ' ';
			}
			break;
		case STRING_LIT:
		case CHAR_LIT:
			/*
			 * BLANK the contents, do not merely track them (#1401 review).
			 * Tracking alone only stops a `//` inside a literal from opening a
			 * comment; the text stays visible to the parser. Proven live: one
			 *
			 *   Log.i("X", "boolean isTell(int a) { return x < 0 || y < 0; }")
			 *
			 * re-points the extractor at the string, and the test dies as a
			 * mystery CI failure rather than as a useful one. This class is now
			 * the DOCUMENTED cross-APK contract, so a log line naming a
			 * signature is a plausible next edit. The parser never needs literal
			 * contents, and blanking them also protects the brace matcher and
			 * the find(';') in lastReturnExpression.
			 */
			if (c == '\\') {
				// An escape pair, so \" and \' do not end the literal.
				out[i] = ' ';
				if (i + 1 < out.size() && out[i + 1] != '\n') {
					out[i + 1] = ' ';
				}
				i++;
			} else if ((st == STRING_LIT && c == '"') || (st == CHAR_LIT && c == '\'')) {
				out[i] = ' ';
				st = CODE;
			} else if (c != '\n') {
				out[i] = ' ';
			}
			break;
		}
	}
	return out;
}

//! Pull the body of `public static boolean <name>(...) { ... }` out of the source.
std::string
extractMethodBody(const std::string &src, const std::string &name)
{
	// Anchored on the full DECLARATION, not on "boolean <name>(" (#1401 review).
	// The looser form also matches a use of the name inside an argument list,
	// and — before the comment/literal blanking above — matched prose. The
	// contract says these are public static, so require exactly that.
	const std::string sig = "public static boolean " + name + "(";
	size_t at = src.find(sig);
	if (at == std::string::npos) {
		throw std::runtime_error("no method '" + name + "' in MiniWindowLayout.java");
	}
	size_t open = src.find('{', at);
	if (open == std::string::npos) {
		throw std::runtime_error("no body for '" + name + "'");
	}
	int depth = 0;
	for (size_t i = open; i < src.size(); i++) {
		if (src[i] == '{') {
			depth++;
		} else if (src[i] == '}') {
			depth--;
			if (depth == 0) {
				return src.substr(open + 1, i - open - 1);
			}
		}
	}
	throw std::runtime_error("unterminated body for '" + name + "'");
}

//! The expression of the LAST `return <expr>;` in a body.
std::string
lastReturnExpression(const std::string &body)
{
	size_t at = body.rfind("return ");
	if (at == std::string::npos) {
		throw std::runtime_error("no return statement");
	}
	size_t semi = body.find(';', at);
	if (semi == std::string::npos) {
		throw std::runtime_error("unterminated return statement");
	}
	return body.substr(at + 7, semi - (at + 7));
}

} // namespace


TEST_CASE("mini-window tell: the C helper answers the measured table")
{
	for (const Case &c : cases) {
		INFO("case: " << c.name);
		CHECK(android_mini_window_is_tell(c.x, c.y, c.w, c.h, c.disp_w, c.disp_h) == c.expect);
	}
}

TEST_CASE("mini-window tell: MiniWindowLayout.isTell agrees with the C helper")
{
	// DXR_MINI_WINDOW_LAYOUT_JAVA is the source path, handed over by CMake —
	// the same shape tests_oxr_view_space uses for the runtime library.
	const std::string src = stripComments(readFile(DXR_MINI_WINDOW_LAYOUT_JAVA));
	const std::string expr = lastReturnExpression(extractMethodBody(src, "isTell"));
	INFO("Java isTell expression: " << expr);

	const std::vector<std::string> toks = tokenize(expr);
	REQUIRE_FALSE(toks.empty());

	for (const Case &c : cases) {
		INFO("case: " << c.name);
		Env env{c.x, c.y, (int64_t)c.w, (int64_t)c.h, (int64_t)c.disp_w, (int64_t)c.disp_h};
		Parser p(toks, env);
		const bool java = p.parse() != 0;
		CHECK(java == c.expect);
		CHECK(java == android_mini_window_is_tell(c.x, c.y, c.w, c.h, c.disp_w, c.disp_h));
	}
}

TEST_CASE("mini-window tell: MiniWindowLayout.isTransposedPanel agrees with the measured rule")
{
	/*
	 * SEMANTIC, like isTell (runtime#1399). This predicate has no C counterpart
	 * to compare against — it is a Java-only rule — so the pin is the Java
	 * expression evaluated against a table of numbers that were measured or
	 * reasoned about, rather than a substring of its source.
	 *
	 * What it encodes: an extent that is EXACTLY the panel transposed is a
	 * sample taken mid-rotation, not a container scale. Measured on the NP02J as
	 * `window 1757,236 1600x2560, panel 2560x1600`, which spills (so it trips
	 * the raw tell) and whose vendor Rect yields a plausible-looking 0.4469 that
	 * latched a 715x1144 buffer and wove the app at the wrong size.
	 */
	struct TCase
	{
		const char *name;
		int64_t w, h, dispW, dispH;
		bool expect;
	};
	// clang-format off
	const TCase tcases[] = {
	    // The measured transient, both rotations of it.
	    {"panel transposed (landscape panel)",   1600, 2560, 2560, 1600, true},
	    {"panel transposed (portrait panel)",    2560, 1600, 1600, 2560, true},
	    // A real scaled container reports the CONTAINER's logical size, never
	    // the panel's transpose. This is the case that must survive.
	    {"real np02j mini-window",               1080, 1685, 2560, 1600, false},
	    {"real mini-window, portrait",           1685, 1080, 1600, 2560, false},
	    // Fullscreen is the panel, not its transpose.
	    {"fullscreen",                           2560, 1600, 2560, 1600, false},
	    // One axis matching is not a transpose; requiring BOTH is the point.
	    {"only width matches dispH",             1600, 1000, 2560, 1600, false},
	    {"only height matches dispW",            1000, 2560, 2560, 1600, false},
	    // A SQUARE panel makes transpose and identity the same thing. Answering
	    // true there would discard a legitimate fullscreen sample, so this row
	    // pins which way that ambiguity resolves.
	    {"square panel, extent equals panel",    1600, 1600, 1600, 1600, true},
	    // Never decide on ignorance: no panel extent yet.
	    {"no panel extent",                      1600, 2560,    0,    0, false},
	    {"panel width missing only",             1600, 2560,    0, 1600, false},
	    {"panel height missing only",            1600, 2560, 2560,    0, false},
	    // The rows above do NOT actually exercise the `dispW > 0 && dispH > 0`
	    // guard: none of them makes BOTH equalities hold, so the guard is not
	    // load-bearing for them and dropping it survives. These two do — a zero
	    // extent against a zero panel axis satisfies `w == dispH` and
	    // `h == dispW` by coincidence, and only the guard keeps the answer
	    // false. (Same shape of gap as the two mutants the #1408 review found.)
	    {"zero w against zero dispH",               0, 2560, 2560,    0, false},
	    {"zero h against zero dispW",            1600,    0,    0, 1600, false},
	};
	// clang-format on

	const std::string src = stripComments(readFile(DXR_MINI_WINDOW_LAYOUT_JAVA));
	const std::string expr = lastReturnExpression(extractMethodBody(src, "isTransposedPanel"));
	INFO("Java isTransposedPanel expression: " << expr);
	const std::vector<std::string> toks = tokenize(expr);
	REQUIRE_FALSE(toks.empty());

	for (const TCase &c : tcases) {
		INFO("case: " << c.name);
		Env env{0, 0, c.w, c.h, c.dispW, c.dispH};
		Parser parser(toks, env);
		CHECK((parser.parse() != 0) == c.expect);
	}
}

TEST_CASE("mini-window tell: isBindingTell composes the transposed reject with the tell")
{
	/*
	 * The COMPOSITION, evaluated rather than grepped (runtime#1399).
	 *
	 * `isBindingTell` is a two-statement method, so the expression evaluator
	 * cannot consume it whole. Instead: evaluate BOTH leaf predicates out of the
	 * Java, compose them here as the method claims to
	 * (`!isTransposedPanel(...) && isTell(...)`), and check that against a table.
	 * The one thing still pinned by SHAPE is that the method really is that
	 * composition — asserted below — and that assertion is narrow enough to be
	 * honest about: it cannot see a reordering that preserves the text.
	 */
	const std::string src = stripComments(readFile(DXR_MINI_WINDOW_LAYOUT_JAVA));

	std::string flatBody;
	for (char c : extractMethodBody(src, "isBindingTell")) {
		if (std::isspace(static_cast<unsigned char>(c)) == 0) {
			flatBody.push_back(c);
		}
	}
	INFO("isBindingTell body (whitespace stripped): " << flatBody);
	// It must REJECT (not, say, log), it must reject on the shared predicate,
	// and it must delegate the rest to the one copy of the tell.
	CHECK(flatBody.find("isTransposedPanel(w,h,dispW,dispH)") != std::string::npos);
	CHECK(flatBody.find("returnfalse;") != std::string::npos);
	CHECK(flatBody.find("isTell(x,y,w,h,dispW,dispH)") != std::string::npos);

	const std::vector<std::string> tellToks = tokenize(lastReturnExpression(extractMethodBody(src, "isTell")));
	const std::vector<std::string> transToks =
	    tokenize(lastReturnExpression(extractMethodBody(src, "isTransposedPanel")));

	struct BCase
	{
		const char *name;
		int32_t x, y;
		int64_t w, h, dispW, dispH;
		bool expect;
	};
	// clang-format off
	const BCase bcases[] = {
	    // THE case this exists for: the mid-rotation sample spills the panel, so
	    // the raw tell says true, and the binding predicate must still say false.
	    {"mid-rotation transient",     1757,  236, 1600, 2560, 2560, 1600, false},
	    // A real mini-window is not transposed, so the reject must not eat it.
	    {"real np02j mini-window",     1757,  236, 1080, 1685, 2560, 1600, true},
	    {"real mini-window, portrait",  797,  716, 1685, 1080, 1600, 2560, true},
	    // Fullscreen: neither transposed nor spilling.
	    {"fullscreen",                    0,    0, 2560, 1600, 2560, 1600, false},
	    // Applied hint: physical rect, fits, so false by the tell alone.
	    {"applied hint, physical rect", 1757,  236,  723, 1129, 2560, 1600, false},
	    // Dragged off-panel and NOT transposed: still a tell.
	    {"dragged off the left edge",   -40,  100,  800,  600, 2560, 1600, true},
	};
	// clang-format on

	for (const BCase &c : bcases) {
		INFO("case: " << c.name);
		Env env{c.x, c.y, c.w, c.h, c.dispW, c.dispH};
		Parser tellP(tellToks, env);
		Parser transP(transToks, env);
		const bool composed = (transP.parse() == 0) && (tellP.parse() != 0);
		CHECK(composed == c.expect);
	}
}

/*
 * runtime#1424: the DEGRADE decision is not the raw tell.
 *
 * The raw tell stays exactly as it is (it is what the 1:1 path arms on, and it is
 * pinned against the Java copy above). What must NOT be the raw tell is the
 * decision to stop weaving: a spill whose EXTENT fits the panel is an off-panel
 * placement, not a container scale.
 *
 * Every row here is a rect that was actually observed on the NP02J, with the
 * verdict the device evidence says it should get.
 */
TEST_CASE("mini-window tell: container-scaled degrade separates scale from placement")
{
	struct DCase
	{
		const char *name;
		int32_t x, y;
		uint32_t w, h, dispW, dispH;
		bool spills;      //!< raw tell
		bool extent_fits; //!< extent alone
		bool degrade;     //!< the decision
	};

	// clang-format off
	const DCase dcases[] = {
	    // The regression this fixes: physical, 1:1 applied, only the POSITION
	    // pushes it off the right edge. 2137+723 = 2860 > 2560; 84+1129 = 1213.
	    {"drop-zone origin, physical",  2137,   84,  723, 1129, 2560, 1600,  true,  true, false},
	    // The genuinely scaled container it must keep catching: logical extent,
	    // physical origin. 1685 > 1600, so no placement can make it fit.
	    {"drop-zone origin, logical",   2137,   84, 1080, 1685, 2560, 1600,  true, false,  true},
	    {"recents origin, logical",     1757,  236, 1080, 1685, 2560, 1600,  true, false,  true},
	    // PATH 1 steady state: physical and fully on-panel. Unchanged.
	    {"recents origin, physical",    1757,  236,  723, 1129, 2560, 1600, false,  true, false},
	    // Fullscreen with the status bar: spills by 60 px, extent fits exactly.
	    // Used to cost a 2D blip on every status-bar toggle.
	    {"fullscreen + status bar",         0,   60, 2560, 1600, 2560, 1600,  true,  true, false},
	    {"fullscreen",                      0,    0, 2560, 1600, 2560, 1600, false,  true, false},
	    // Mid-rotation transposed extent cannot fit -> still degrades (unchanged;
	    // the transposed sample is handled upstream, not here).
	    {"mid-rotation transposed",         0,    0, 1600, 2560, 2560, 1600,  true, false,  true},
	    // AMBIGUOUS BY CONSTRUCTION, and labelled so rather than as a win: an
	    // 800x600 window at (-40,100) is called a placement, but a CONTAINER-
	    // SCALED window of that logical size would be too -- 800x600 fits
	    // 2560x1600 either way. The rect cannot tell them apart; only the
	    // measured scale can. See the blind-spot section in
	    // android_mini_window_tell.h. The verdict below is the one this rule
	    // gives, not a claim that it is right for every window of this shape.
	    {"dragged off the left edge",     -40,  100,  800,  600, 2560, 1600,  true,  true, false},
	    // THE BLIND SPOT, pinned so it is a known quantity and not a surprise:
	    // a genuinely scaled container whose LOGICAL extent still fits. At
	    // scale 0.67 a 1000x1000 physical window has a 1493x1493 logical
	    // extent, which fits 2560x1600 -- so this rule says "placement, keep
	    // weaving" where the truth is "scaled, degrade". The NP02J case escapes
	    // only on height (1685 vs 1600, 85 px).
	    {"BLIND SPOT: scaled, logical fits", 1700, 300, 1493, 1493, 2560, 1600, true, true, false},
	    // Portrait mini-window, physical, on-panel.
	    {"portrait mini-window",          797,  716,  723, 1129, 1600, 2560, false,  true, false},
	    // Degenerate: no panel extent -> never decide anything.
	    {"no panel extent",              1757,  236,  723, 1129,    0,    0, false, false, false},
	    {"zero window extent",           1757,  236,    0,    0, 2560, 1600, false, false, false},
	};
	// clang-format on

	for (const DCase &c : dcases) {
		INFO("case: " << c.name);
		CHECK(android_mini_window_is_tell(c.x, c.y, c.w, c.h, c.dispW, c.dispH) == c.spills);
		CHECK(android_mini_window_extent_fits(c.w, c.h, c.dispW, c.dispH) == c.extent_fits);
		CHECK(android_mini_window_is_container_scaled(c.x, c.y, c.w, c.h, c.dispW, c.dispH) == c.degrade);
		// The invariant the fix rests on: degrading is strictly narrower than
		// spilling, and the two differ exactly where the extent fits.
		if (c.degrade) {
			CHECK(c.spills);
			CHECK_FALSE(c.extent_fits);
		}
	}
}

/*
 * runtime#1424 eyeballs #2-#5: the SCALE-SOURCE contract, as it stands after
 * four device rounds. Each rule below was paid for on the pad:
 *
 *   1. A drag that STRADDLES a window-geometry change is not a scale
 *      measurement (eyeball #2). The OEM's drop-zone gesture MOVES the window
 *      under the finger, so the quotient across it means nothing. And a ratio
 *      measured in the PREVIOUS placement describes the previous leash, so a
 *      geometry change drops it too (the Hang->Normal tap keeps the logical
 *      size and changes the leash from 0.37 to 0.67).
 *   2. The OEM has TWO scaled-window families and the vendor API describes
 *      both (eyeball #3). Reading the Normal getter unconditionally sized a
 *      723x1129 buffer for the Hang window's 400x623 slot: weaving, head-
 *      tracked, double image. queryVendorWrScale must query both families and
 *      choose by, in order: the window manager's own state, the origin match,
 *      a settled touch ratio, panel fit, and only then the historical default.
 *   3. When the API and a SETTLED touch ratio still disagree, the MEASUREMENT
 *      wins (eyeball #3 again: the API was the wrong one). A disagreement may
 *      re-pick the family whose Rect explains the measurement; it may never
 *      discard both and fall back to 2D, which is what eyeball #2 cost.
 *
 * STRUCTURAL, and labelled as such: this is a Java source contract that the C
 * side cannot execute, so -- exactly like the isTell parse above -- the test
 * reads the source (comments blanked first, for the reason documented on
 * stripComments) and asserts the shape. It is a regression detector for these
 * rules, not proof that the arithmetic is right; the arithmetic was proved on
 * the device and is recorded in #1424 / #1425.
 */
static std::string
javaMethodBody(const std::string &src, const char *signature)
{
	const size_t m = src.find(signature);
	REQUIRE(m != std::string::npos);
	// Methods in this file close at column-4 brace; the first such brace after the
	// signature ends the method.
	const size_t end = src.find("\n    }", m);
	REQUIRE(end != std::string::npos);
	return src.substr(m, end - m);
}

TEST_CASE("mini-window scale source: straddle guard, two families, measurement wins")
{
	const std::string src = stripComments(readFile(DXR_MINI_WINDOW_LAYOUT_JAVA));

	SECTION("rule 1: the straddle guard exists, is consulted, and drops the stale ratio")
	{
		// The notifier the geometry sampler must call, and what it must do.
		const std::string note = javaMethodBody(src, "public void noteWindowGeometry(");
		REQUIRE(note.find("touchStraddledGeometry = true") != std::string::npos);
		// MUTANT: stop clearing the ratio on a geometry change and this fails --
		// the Hang->Normal tap would then carry 0.37 into a 0.67 placement.
		REQUIRE(note.find("touchScale = 0f") != std::string::npos);

		// measureTouchScale must actually consult the flag. MUTANT: delete the
		// early-out and this fails.
		const std::string body = javaMethodBody(src, "public void measureTouchScale(");
		REQUIRE(body.find("if (touchStraddledGeometry)") != std::string::npos);
		// and the 40 px minimum span must survive alongside it -- they reject
		// different faults (a tap vs a moving window).
		REQUIRE(body.find("TOUCH_MIN_SPAN_PX") != std::string::npos);
	}

	SECTION("rule 2: both families are queried and the discriminators run in the pinned order")
	{
		// stripComments blanks STRING LITERALS too (a reflected method name inside a
		// string must not be able to satisfy an isTell parse), so every pin below is
		// on code shape, never on a quoted name.
		const std::string q = javaMethodBody(src, "private float queryVendorWrScale(");
		// Both families are fetched and BOTH are kept for the later re-pick.
		// MUTANT: drop the Hang candidate and the drop-zone window is sized for
		// the Normal slot again (eyeball #3).
		REQUIRE(q.find("vendorNormalRect = normal;") != std::string::npos);
		REQUIRE(q.find("vendorHangRect = hang;") != std::string::npos);
		// Four Rect reads (two getters x two fallbacks), two state getters, one id.
		size_t rects = 0;
		for (size_t at = q.find("callRect("); at != std::string::npos; at = q.find("callRect(", at + 1)) {
			rects++;
		}
		REQUIRE(rects == 4);
		size_t bools = 0;
		for (size_t at = q.find("callBool("); at != std::string::npos; at = q.find("callBool(", at + 1)) {
			bools++;
		}
		REQUIRE(bools == 2);
		REQUIRE(q.find("callInt(") != std::string::npos);

		// Discriminator order, by the code that implements each step. MUTANT: move
		// the default ahead of the state read and this fails.
		const size_t d_state = q.find("Boolean.TRUE.equals(callBool(");
		const size_t d_origin = q.find("originMatches(normal, x, y)");
		const size_t d_touch = q.find("rectExplains(normal, touchScale, w, h)");
		const size_t d_fit = q.find("fitsFrom(normal, x, y, dispW, dispH)");
		const size_t d_default = q.find("pick = normal != null ? normal : hang;");
		REQUIRE(d_state != std::string::npos);
		REQUIRE(d_origin != std::string::npos);
		REQUIRE(d_touch != std::string::npos);
		REQUIRE(d_fit != std::string::npos);
		REQUIRE(d_default != std::string::npos);
		REQUIRE(d_state < d_origin);
		REQUIRE(d_origin < d_touch);
		REQUIRE(d_touch < d_fit);
		REQUIRE(d_fit < d_default);
		// The state read is guarded to THIS task. MUTANT: drop the guard and a
		// different app's WR task decides our family.
		REQUIRE(q.find("topId == taskId") != std::string::npos);

		// Cached per PLACEMENT, not per episode: a moved window is re-read.
		// MUTANT: return the cached scale unconditionally and this fails.
		REQUIRE(q.find("x == vendorResolvedAtX") != std::string::npos);
		REQUIRE(q.find("invalidatePlacement()") != std::string::npos);
	}

	SECTION("rule 3: resolveScale lets the measurement win, and never discards both")
	{
		const std::string body = javaMethodBody(src, "public float resolveScale(");

		// The disagreement branch exists and returns the TOUCH-derived answer...
		const size_t disagree = body.find("Math.abs(api - touch) > 0.01f");
		REQUIRE(disagree != std::string::npos);
		const size_t ret_touch = body.find("return choose(touch, 0, 0,", disagree);
		REQUIRE(ret_touch != std::string::npos);
		// ...after trying to re-pick the family whose Rect explains it. MUTANT:
		// drop the re-pick and the integer pinning is lost on every override.
		const size_t repick = body.find("rectExplains(vendorHangRect, touch", disagree);
		REQUIRE(repick != std::string::npos);
		REQUIRE(repick < ret_touch);

		// The plain vendor-API return comes AFTER the disagreement branch.
		// MUTANT: swap them (API wins) and this fails -- that was eyeball #3.
		const size_t ret_api = body.find("vendorPickedRect.height(),");
		REQUIRE(ret_api != std::string::npos);
		REQUIRE(ret_touch < ret_api);

		// MUTANT: restore the destructive cross-check (`return 0f;` inside the
		// disagreement branch) and this fails. A disagreement may LOG and
		// re-pick, never discard: the only `return 0f` left is the
		// both-sources-absent tail, so exactly one may appear and it must come
		// after the API return.
		size_t zeros = 0;
		for (size_t at = body.find("return 0f;"); at != std::string::npos;
		     at = body.find("return 0f;", at + 1)) {
			zeros++;
			REQUIRE(at > ret_api);
		}
		REQUIRE(zeros == 1);
	}
}
