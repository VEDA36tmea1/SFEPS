#ifndef TEXT_UTILS_H
#define TEXT_UTILS_H

#include <string>
#include <string_view>

std::string trim_copy(const std::string& input);
std::string to_lower_copy(std::string_view raw);
std::string sanitize_for_log(const std::string& input);

#endif
