/*
 * json_writer.h — Simple JSON writer for dwg_to_json.
 */

#ifndef JSON_WRITER_H
#define JSON_WRITER_H

#include <cstdio>
#include <string>
#include <vector>
#include <map>

// JSON string escaping helper
static std::string json_escape_string(const char* s) {
    std::string out;
    out.reserve(strlen(s) + 4);
    for (const char* p = s; *p; ++p) {
        switch (*p) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b";  break;
            case '\f': out += "\\f";  break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (static_cast<unsigned char>(*p) < 0x20) {
                    char hex[8];
                    std::snprintf(hex, sizeof(hex), "\\u%04x", static_cast<unsigned char>(*p));
                    out += hex;
                } else {
                    out += *p;
                }
        }
    }
    return out;
}

class JsonWriter {
public:
    JsonWriter(FILE* out) : m_out(out), m_first(true), m_indent(0) {}

    void start_object() {
        if (!m_first) fprintf(m_out, ",\n");
        print_indent();
        fprintf(m_out, "{");
        m_indent++;
        m_first = true;
    }

    void end_object() {
        m_indent--;
        fprintf(m_out, "\n");
        print_indent();
        fprintf(m_out, "}");
        m_first = false;
    }

    void start_array(const char* key) {
        if (!m_first) fprintf(m_out, ",\n");
        print_indent();
        fprintf(m_out, "\"%s\": [", key);
        m_indent++;
        m_first = true;
    }

    void end_array() {
        m_indent--;
        fprintf(m_out, "\n");
        print_indent();
        fprintf(m_out, "]");
        m_first = false;
    }

    // key-value where value is an object (resets first so inner writes don't get comma)
    void start_object(const char* key) {
        if (!m_first) fprintf(m_out, ",\n");
        print_indent();
        fprintf(m_out, "\"%s\": {", key);
        m_indent++;
        m_first = true;
    }

    void write_key(const char* key) {
        if (!m_first) fprintf(m_out, ",\n");
        print_indent();
        fprintf(m_out, "\"%s\": ", key);
        m_first = false;
    }

    void write_string(const char* key, const char* value) {
        if (!m_first) fprintf(m_out, ",\n");
        print_indent();
        std::string esc = json_escape_string(value);
        fprintf(m_out, "\"%s\": \"%s\"", key, esc.c_str());
        m_first = false;
    }

    void write_string_value(const char* value) {
        if (!m_first) fprintf(m_out, ",");
        std::string esc = json_escape_string(value);
        fprintf(m_out, "\"%s\"", esc.c_str());
        m_first = false;
    }

    void write_int(const char* key, int value) {
        if (!m_first) fprintf(m_out, ",\n");
        print_indent();
        fprintf(m_out, "\"%s\": %d", key, value);
        m_first = false;
    }

    void write_int_value(int value) {
        if (!m_first) fprintf(m_out, ",");
        fprintf(m_out, "%d", value);
        m_first = false;
    }

    void write_double(const char* key, double value) {
        if (!m_first) fprintf(m_out, ",\n");
        print_indent();
        fprintf(m_out, "\"%s\": %.6g", key, value);
        m_first = false;
    }

    void write_double_value(double value) {
        if (!m_first) fprintf(m_out, ",");
        fprintf(m_out, "%.6g", value);
        m_first = false;
    }

    void write_bool(const char* key, bool value) {
        if (!m_first) fprintf(m_out, ",\n");
        print_indent();
        fprintf(m_out, "\"%s\": %s", key, value ? "true" : "false");
        m_first = false;
    }

    void write_color_array(const char* key, int r, int g, int b) {
        if (!m_first) fprintf(m_out, ",\n");
        print_indent();
        fprintf(m_out, "\"%s\": [%d, %d, %d]", key, r, g, b);
        m_first = false;
    }

    void write_point(const char* key, double x, double y, double z = 0.0) {
        if (!m_first) fprintf(m_out, ",\n");
        print_indent();
        if (z == 0.0) {
            fprintf(m_out, "\"%s\": [%.6g, %.6g]", key, x, y);
        } else {
            fprintf(m_out, "\"%s\": [%.6g, %.6g, %.6g]", key, x, y, z);
        }
        m_first = false;
    }

    void reset_first() { m_first = true; }
    FILE* file() { return m_out; }
    void set_first(bool v) { m_first = v; }

    // Write a raw JSON value (number, array, etc.) without quotes
    void write_raw(const char* key, const char* value) {
        if (!m_first) fprintf(m_out, ",\n");
        print_indent();
        fprintf(m_out, "\"%s\": %s", key, value);
        m_first = false;
    }

private:
    FILE* m_out;
    bool m_first;
    int m_indent;

    void print_indent() {
        for (int i = 0; i < m_indent; i++) fprintf(m_out, "  ");
    }
};

#endif // JSON_WRITER_H
