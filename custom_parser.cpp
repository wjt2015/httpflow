#include "custom_parser.h"
#include "util.h"

custom_parser::custom_parser() : gzip_flag(false) {
    complete_flag[HTTP_REQUEST] = false;
    complete_flag[HTTP_RESPONSE] = false;
    next_seq[HTTP_REQUEST] = 0;
    next_seq[HTTP_RESPONSE] = 0;
    http_parser_init(&parser, HTTP_REQUEST);
    parser.data = this;

    http_parser_settings_init(&settings);
    settings.on_url = on_url;
    settings.on_header_field = on_header_field;
    settings.on_header_value = on_header_value;
    settings.on_headers_complete = on_headers_complete;
    settings.on_body = on_body;
    settings.on_message_complete = on_message_complete;
}

bool custom_parser::parse(const struct packet_info &packet, enum http_parser_type type) {
    if (parser.type != type) {
        http_parser_init(&parser, type);
    }
    size_t orig_size = 0;
    std::string *str = NULL;
    if (parser.type == HTTP_REQUEST || parser.type == HTTP_RESPONSE) {
        orig_size = raw[parser.type].size();
        str = &raw[parser.type];
        if (next_seq[parser.type] != 0 && packet.seq != next_seq[parser.type]) {
            if (packet.seq < next_seq[parser.type]) {
                // ignore retransmission
                return true;
            } else {
                out_of_order_packet[parser.type].insert(std::make_pair(packet.seq, packet.body));
            }
        } else {
            raw[parser.type].append(packet.body);
            next_seq[parser.type] = packet.seq + packet.body.size();
        }
        while (!out_of_order_packet[parser.type].empty()) {
            const std::map<uint32_t, std::string>::iterator &iterator =
                    out_of_order_packet[parser.type].find(next_seq[parser.type]);
            if (iterator != out_of_order_packet[parser.type].end()) {
                raw[parser.type].append(iterator->second);
                next_seq[parser.type] += iterator->second.size();
                out_of_order_packet[parser.type].erase(iterator);
            } else {
                break;
            }
        }
    }

    if (str->size() > orig_size) {
        size_t parse_bytes = http_parser_execute(&parser, &settings, str->c_str() + orig_size, str->size() - orig_size);
        return parse_bytes > 0 && HTTP_PARSER_ERRNO(&parser) == HPE_OK;
    }
    return true;
}

std::string custom_parser::get_response_body() const {
    return body[HTTP_RESPONSE];
}

void custom_parser::set_addr(const std::string &src_addr, const std::string &dst_addr) {
    this->address[HTTP_REQUEST] = src_addr;
    this->address[HTTP_RESPONSE] = dst_addr;
}

int custom_parser::on_url(http_parser *parser, const char *at, size_t length) {
    custom_parser *self = reinterpret_cast<custom_parser *>(parser->data);
    self->url.assign(at, length);
    self->method.assign(http_method_str(static_cast<enum http_method>(parser->method)));
    return 0;
};

int custom_parser::on_header_field(http_parser *parser, const char *at, size_t length) {
    custom_parser *self = reinterpret_cast<custom_parser *>(parser->data);
    self->temp_header_field.assign(at, length);
    for (size_t i = 0; i < length; ++i) {
        if (at[i] >= 'A' && at[i] <= 'Z') {
            self->temp_header_field[i] = at[i] ^ (char) 0x20;
        }
    }
    return 0;
}

int custom_parser::on_header_value(http_parser *parser, const char *at, size_t length) {
    custom_parser *self = reinterpret_cast<custom_parser *>(parser->data);
    if (parser->type == HTTP_RESPONSE) {
        if (self->temp_header_field == "content-encoding" && std::strstr(at, "gzip")) {
            self->gzip_flag = true;
        }
    } else {
        if (self->temp_header_field == "host") {
            self->host.assign(at, length);
        }
    }
    // std::cout << self->temp_header_field <<  ":" << std::string(at, length) << std::endl;
    return 0;
}

int custom_parser::on_headers_complete(http_parser *parser) {
    if (parser->type == HTTP_REQUEST || parser->type == HTTP_RESPONSE) {
        custom_parser *self = reinterpret_cast<custom_parser *>(parser->data);
        self->header[parser->type] = self->raw[parser->type].substr(0, parser->nread);
    }
    return 0;
}

int custom_parser::on_body(http_parser *parser, const char *at, size_t length) {
    if (parser->type == HTTP_REQUEST || parser->type == HTTP_RESPONSE) {
        custom_parser *self = reinterpret_cast<custom_parser *>(parser->data);
        self->body[parser->type].append(at, length);
    }
    return 0;
}

int custom_parser::on_message_complete(http_parser *parser) {
    custom_parser *self = reinterpret_cast<custom_parser *>(parser->data);
    if (parser->type == HTTP_REQUEST || parser->type == HTTP_RESPONSE) {
        self->complete_flag[parser->type] = true;
    }
    if (self->gzip_flag) {
        std::string new_body;
        if (gzip_decompress(self->body[HTTP_RESPONSE], new_body)) {
            self->body[HTTP_RESPONSE] = new_body;
        } else {
            std::cerr << ANSI_COLOR_RED << "[decompress error]" << ANSI_COLOR_RESET << std::endl;
        }
    }
    return 0;
}

bool custom_parser::filter_url(const pcre *url_filter_re, const pcre_extra *url_filter_extra, const std::string &url) {
    if (!url_filter_re) return true;
    int ovector[30];
    int rc = pcre_exec(url_filter_re, url_filter_extra, url.c_str(), url.size(), 0, 0, ovector, 30);
    return rc >= 0;
}

void custom_parser::save_http_request(const pcre *url_filter_re, const pcre_extra *url_filter_extra,
                                      const std::string &output_path, const std::string &join_addr) {
    std::string host_with_url = host + url;
    if (!filter_url(url_filter_re, url_filter_extra, host_with_url)) {
        return;
    }
    std::cout << ANSI_COLOR_CYAN << address[HTTP_REQUEST] << " -> " << address[HTTP_RESPONSE] << ANSI_COLOR_RESET
              << std::endl;
    if (!output_path.empty()) {
        std::string save_filename = output_path + "/" + host;
        std::ofstream out(save_filename.c_str(), std::ios::app | std::ios::out);
        if (out.is_open()) {
            out << *this << std::endl;
            out.close();
        } else {
            std::cerr << "ofstream [" << save_filename << "] is not opened." << std::endl;
            out.close();
            exit(1);
        }
    } else {
        std::cout << *this << std::endl;
    }
}

std::ostream &operator<<(std::ostream &out, const custom_parser &parser) {
    out << ANSI_COLOR_GREEN
        << parser.header[HTTP_REQUEST]
        << ANSI_COLOR_RESET;
    if (!is_atty || is_plain_text(parser.body[HTTP_REQUEST])) {
        out << parser.body[HTTP_REQUEST];
    } else {
        out << ANSI_COLOR_RED << "[binary request body]" << ANSI_COLOR_RESET;
    }
    out << std::endl
        << ANSI_COLOR_BLUE
        << parser.header[HTTP_RESPONSE]
        << ANSI_COLOR_RESET;
    if (parser.body[HTTP_RESPONSE].empty()) {
        out << ANSI_COLOR_RED << "[empty response body]" << ANSI_COLOR_RESET;
    } else if (!is_atty || is_plain_text(parser.body[HTTP_RESPONSE])) {
        out << parser.body[HTTP_RESPONSE];
    } else {
        out << ANSI_COLOR_RED << "[binary response body]" << ANSI_COLOR_RESET;
    }
    return out;
}

std::ofstream &operator<<(std::ofstream &out, const custom_parser &parser) {
    out << parser.header[HTTP_REQUEST]
        << parser.body[HTTP_REQUEST]
        << parser.header[HTTP_RESPONSE]
        << parser.body[HTTP_RESPONSE];
    return out;
}
