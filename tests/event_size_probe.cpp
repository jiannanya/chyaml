#include "chyaml.hpp"

#include <string_view>

int main() {
    constexpr std::string_view input =
        "device:\n"
        "  name: sensor\n"
        "  enabled: true\n"
        "  samples: [1, 2, 3]\n";

    chyaml::event_parser parser;
    if (!parser.reset_borrowed(input)) return 1;
    chyaml::event event;
    unsigned scalars = 0;
    while (parser.next(event) == chyaml::event_status::event)
        if (event.type == chyaml::event_type::scalar) ++scalars;
    return parser.error() || scalars != 9 ? 2 : 0;
}
