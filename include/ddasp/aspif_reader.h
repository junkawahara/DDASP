#ifndef DDASP_ASPIF_READER_H
#define DDASP_ASPIF_READER_H

/**
 * @file aspif_reader.h
 * @brief Reader for the aspif intermediate format emitted by gringo
 *        (`gringo --output=intermediate`).
 */

#include <istream>
#include <stdexcept>
#include <string>
#include <vector>

#include "ddasp/program.h"

namespace ddasp {

/** @brief Error raised on malformed aspif input. */
class AspifError : public std::runtime_error {
public:
    AspifError(const std::string& msg, int line)
        : std::runtime_error("aspif line " + std::to_string(line) + ": " + msg),
          line_(line) {}
    int line() const { return line_; }

private:
    int line_;
};

/**
 * @brief Parse an aspif program from a stream.
 *
 * Supported statements: rule (1), minimize (2), projection (3), output (4),
 * external (5), comment (10) and end (0). Assumption (6), heuristic (7),
 * edge (8) and theory (9) statements are skipped with a warning.
 *
 * Incremental programs (several steps each terminated by `0`) are read as a
 * single accumulated program.
 *
 * @param in Input stream.
 * @param warnings If non-null, receives one message per skipped statement.
 * @throws AspifError on malformed input.
 */
Program read_aspif(std::istream& in, std::vector<std::string>* warnings = 0);

/** @brief Convenience overload: parse aspif text held in a string. */
Program read_aspif_string(const std::string& text, std::vector<std::string>* warnings = 0);

/** @brief Convenience overload: parse an aspif file. Throws std::runtime_error if it cannot be opened. */
Program read_aspif_file(const std::string& path, std::vector<std::string>* warnings = 0);

}  // namespace ddasp

#endif  // DDASP_ASPIF_READER_H
