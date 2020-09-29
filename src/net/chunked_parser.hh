/* -*-mode:c++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */

#ifndef CHUNKED_BODY_PARSER_HH
#define CHUNKED_BODY_PARSER_HH

#include "util/exception.h"

namespace pbrt {

class BodyParser {
  public:
    /* possible return values from body parser:
        - entire string belongs to body
        - only some of string (0 bytes to n bytes) belongs to body */

    virtual std::string::size_type read(const std::string& str) = 0;

    /* does message become complete upon EOF in body? */
    virtual bool eof() const = 0;

    virtual ~BodyParser() {}
};

/* used for RFC 2616 4.4 "rule 5" responses -- terminated only by EOF */
class Rule5BodyParser : public BodyParser {
  public:
    /* all of buffer always belongs to body */
    std::string::size_type read(const std::string&) override {
        return std::string::npos;
    }

    /* does message become complete upon EOF in body? */
    /* when there was no content-length header on a response, answer is yes */
    bool eof() const override { return true; }
};

class ChunkedBodyParser : public BodyParser {
  private:
    std::string::size_type compute_ack_size(const std::string& haystack,
                                            const std::string& needle,
                                            std::string::size_type input_size);
    uint32_t get_chunk_size(const std::string& chunk_hdr) const;
    std::string parser_buffer_{""};
    uint32_t current_chunk_size_{0};
    std::string::size_type acked_so_far_{0};
    std::string::size_type parsed_so_far_{0};
    enum { CHUNK_HDR, CHUNK, TRAILER } state_{CHUNK_HDR};
    const bool trailers_enabled_{false};

  public:
    std::string::size_type read(const std::string&) override;

    /* Follow item 2, Section 4.4 of RFC 2616 */
    bool eof() const override { return true; }

    ChunkedBodyParser(bool t_trailers_enabled)
        : trailers_enabled_(t_trailers_enabled) {}
};

}  // namespace pbrt

#endif /* CHUNKED_BODY_PARSER_HH */
