#include <eosio/roborovski_writer_plugin/abi_json.hpp>

#include <vector>

namespace eosio::roborovski_writer {

namespace {

struct decode_error {};

class reader {
public:
   reader(const char* data, size_t len)
      : p(reinterpret_cast<const uint8_t*>(data))
      , end(p + len) {}

   bool remaining() const { return p < end; }

   uint8_t u8() {
      if (p >= end)
         throw decode_error{};
      return *p++;
   }

   uint16_t u16() {
      uint16_t v = u8();
      v |= static_cast<uint16_t>(u8()) << 8;
      return v;
   }

   uint64_t u64() {
      uint64_t v = 0;
      for (int i = 0; i < 8; ++i)
         v |= static_cast<uint64_t>(u8()) << (8 * i);
      return v;
   }

   uint64_t uvarint() {
      uint64_t x = 0;
      unsigned s = 0;
      for (int i = 0; i < 10; ++i) {
         uint8_t b = u8();
         if (b < 0x80) {
            if (i == 9 && b > 1)
               throw decode_error{};
            return x | static_cast<uint64_t>(b) << s;
         }
         x |= static_cast<uint64_t>(b & 0x7f) << s;
         s += 7;
      }
      throw decode_error{};
   }

   std::string bytes() {
      uint64_t n = uvarint();
      if (n > static_cast<uint64_t>(end - p))
         throw decode_error{};
      std::string out(reinterpret_cast<const char*>(p), static_cast<size_t>(n));
      p += n;
      return out;
   }

private:
   const uint8_t* p;
   const uint8_t* end;
};

uint64_t array_length(reader& r) { return r.uvarint(); }

void append_uint(std::string& out, uint64_t v) { out += std::to_string(v); }

void append_go_uint64(std::string& out, uint64_t v) {
   if (v > 0xffffffffull) {
      out += '"';
      out += std::to_string(v);
      out += '"';
   } else {
      out += std::to_string(v);
   }
}

void append_str(std::string& out, const std::string& s) { out += go_json_string(s); }

template <typename F>
void append_array(std::string& out, reader& r, F&& element) {
   uint64_t n = array_length(r);
   if (n == 0) {
      out += "null";
      return;
   }
   out += '[';
   for (uint64_t i = 0; i < n; ++i) {
      if (i)
         out += ',';
      element();
   }
   out += ']';
}

template <typename F>
void append_omitempty_array(std::string& out, reader& r, const char* key, F&& element) {
   uint64_t n = array_length(r);
   if (n == 0)
      return;
   out += ",\"";
   out += key;
   out += "\":[";
   for (uint64_t i = 0; i < n; ++i) {
      if (i)
         out += ',';
      element();
   }
   out += ']';
}

void append_string_array(std::string& out, reader& r) {
   append_array(out, r, [&] { append_str(out, r.bytes()); });
}

void append_hex(std::string& out, const std::string& bytes) {
   static const char* digits = "0123456789abcdef";
   out += '"';
   for (unsigned char c : bytes) {
      out += digits[c >> 4];
      out += digits[c & 0xf];
   }
   out += '"';
}

std::string decode_abi(reader& r) {
   std::string out;
   out.reserve(4096);
   out += "{\"version\":";
   append_str(out, r.bytes());

   out += ",\"types\":";
   append_array(out, r, [&] {
      out += "{\"new_type_name\":";
      append_str(out, r.bytes());
      out += ",\"type\":";
      append_str(out, r.bytes());
      out += '}';
   });

   out += ",\"structs\":";
   append_array(out, r, [&] {
      out += "{\"name\":";
      append_str(out, r.bytes());
      out += ",\"base\":";
      append_str(out, r.bytes());
      out += ",\"fields\":";
      append_array(out, r, [&] {
         out += "{\"name\":";
         append_str(out, r.bytes());
         out += ",\"type\":";
         append_str(out, r.bytes());
         out += '}';
      });
      out += '}';
   });

   out += ",\"actions\":";
   append_array(out, r, [&] {
      out += "{\"name\":";
      append_str(out, name_to_string(r.u64()));
      out += ",\"type\":";
      append_str(out, r.bytes());
      out += ",\"ricardian_contract\":";
      append_str(out, r.bytes());
      out += '}';
   });

   out += ",\"tables\":";
   append_array(out, r, [&] {
      out += "{\"name\":";
      append_str(out, name_to_string(r.u64()));
      out += ",\"index_type\":";
      append_str(out, r.bytes());
      out += ",\"key_names\":";
      append_string_array(out, r);
      out += ",\"key_types\":";
      append_string_array(out, r);
      out += ",\"type\":";
      append_str(out, r.bytes());
      out += '}';
   });

   out += ",\"ricardian_clauses\":";
   append_array(out, r, [&] {
      out += "{\"id\":";
      append_str(out, r.bytes());
      out += ",\"body\":";
      append_str(out, r.bytes());
      out += '}';
   });

   append_omitempty_array(out, r, "error_messages", [&] {
      out += "{\"error_code\":";
      append_go_uint64(out, r.u64());
      out += ",\"error_msg\":";
      append_str(out, r.bytes());
      out += '}';
   });

   append_omitempty_array(out, r, "abi_extensions", [&] {
      out += "{\"type\":";
      append_uint(out, r.u16());
      out += ",\"data\":";
      append_hex(out, r.bytes());
      out += '}';
   });

   if (r.remaining()) {
      append_omitempty_array(out, r, "variants", [&] {
         out += "{\"name\":";
         append_str(out, r.bytes());
         out += ",\"types\":";
         append_string_array(out, r);
         out += '}';
      });
   }

   out += '}';
   return out;
}

size_t utf8_rune_width(const unsigned char* s, size_t n) {
   unsigned char c = s[0];
   if (c < 0x80)
      return 1;
   auto cont = [&](size_t i, unsigned char lo = 0x80, unsigned char hi = 0xBF) {
      return i < n && s[i] >= lo && s[i] <= hi;
   };
   if (c >= 0xC2 && c <= 0xDF)
      return cont(1) ? 2 : 0;
   if (c == 0xE0)
      return cont(1, 0xA0, 0xBF) && cont(2) ? 3 : 0;
   if ((c >= 0xE1 && c <= 0xEC) || c == 0xEE || c == 0xEF)
      return cont(1) && cont(2) ? 3 : 0;
   if (c == 0xED)
      return cont(1, 0x80, 0x9F) && cont(2) ? 3 : 0;
   if (c == 0xF0)
      return cont(1, 0x90, 0xBF) && cont(2) && cont(3) ? 4 : 0;
   if (c >= 0xF1 && c <= 0xF3)
      return cont(1) && cont(2) && cont(3) ? 4 : 0;
   if (c == 0xF4)
      return cont(1, 0x80, 0x8F) && cont(2) && cont(3) ? 4 : 0;
   return 0;
}

} // namespace

std::string name_to_string(uint64_t value) {
   static const char* charmap = ".12345abcdefghijklmnopqrstuvwxyz";
   std::string        str(13, '.');
   uint64_t           tmp = value;
   for (uint32_t i = 0; i <= 12; ++i) {
      char c       = charmap[tmp & (i == 0 ? 0x0f : 0x1f)];
      str[12 - i]  = c;
      tmp        >>= (i == 0 ? 4 : 5);
   }
   auto last = str.find_last_not_of('.');
   return last == std::string::npos ? std::string() : str.substr(0, last + 1);
}

std::string go_json_string(const std::string& s) {
   static const char* hex = "0123456789abcdef";
   std::string        out;
   out.reserve(s.size() + 2);
   out += '"';
   const auto* p = reinterpret_cast<const unsigned char*>(s.data());
   size_t      n = s.size();
   for (size_t i = 0; i < n;) {
      unsigned char c = p[i];
      if (c < 0x80) {
         switch (c) {
         case '"': out += "\\\""; break;
         case '\\': out += "\\\\"; break;
         case '\b': out += "\\b"; break;
         case '\f': out += "\\f"; break;
         case '\n': out += "\\n"; break;
         case '\r': out += "\\r"; break;
         case '\t': out += "\\t"; break;
         case '<':
         case '>':
         case '&':
            out += "\\u00";
            out += hex[c >> 4];
            out += hex[c & 0xf];
            break;
         default:
            if (c < 0x20) {
               out += "\\u00";
               out += hex[c >> 4];
               out += hex[c & 0xf];
            } else {
               out += static_cast<char>(c);
            }
         }
         ++i;
         continue;
      }
      size_t w = utf8_rune_width(p + i, n - i);
      if (w == 0) {
         out += "\\ufffd";
         ++i;
         continue;
      }
      if (w == 3 && p[i] == 0xE2 && p[i + 1] == 0x80 && (p[i + 2] == 0xA8 || p[i + 2] == 0xA9)) {
         out += p[i + 2] == 0xA8 ? "\\u2028" : "\\u2029";
      } else {
         out.append(reinterpret_cast<const char*>(p + i), w);
      }
      i += w;
   }
   out += '"';
   return out;
}

std::optional<std::string> abi_binary_to_json(const char* data, size_t len) {
   try {
      reader r(data, len);
      return decode_abi(r);
   } catch (const decode_error&) {
      return std::nullopt;
   }
}

std::optional<parsed_setabi> parse_setabi(const char* data, size_t len) {
   if (len < 8)
      return std::nullopt;
   parsed_setabi out;
   for (int i = 0; i < 8; ++i)
      out.contract |= static_cast<uint64_t>(static_cast<uint8_t>(data[i])) << (8 * i);

   size_t offset = 8;
   if (offset >= len)
      return std::nullopt;

   uint32_t abi_len = 0;
   unsigned shift   = 0;
   size_t   read    = 0;
   for (;;) {
      if (offset + read >= len)
         return std::nullopt;
      uint8_t b = static_cast<uint8_t>(data[offset + read]);
      ++read;
      if (shift < 32)
         abi_len |= static_cast<uint32_t>(b & 0x7f) << shift;
      shift += 7;
      if ((b & 0x80) == 0)
         break;
   }
   offset += read;

   if (offset + abi_len > len)
      return std::nullopt;
   if (abi_len == 0)
      return out;

   auto json = abi_binary_to_json(data + offset, abi_len);
   if (!json)
      return std::nullopt;
   out.json = std::move(*json);
   return out;
}

} // namespace eosio::roborovski_writer
