//
// Aspia Project
// Copyright (C) 2020 Dmitry Chapyshev <dmitry@aspia.ru>
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program. If not, see <https://www.gnu.org/licenses/>.
//

#include "base/version.h"

#include "base/logging.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"

#include <algorithm>
#include <cstddef>
#include <cstdlib>

namespace base {

namespace {

std::vector<std::string_view> splitString(std::string_view str, char separator)
{
    std::vector<std::string_view> result;

    if (str.empty())
        return result;

    std::size_t start = 0;

    while (start != std::string_view::npos)
    {
        std::size_t end = str.find_first_of(separator, start);

        std::string_view piece;

        if (end == std::string_view::npos)
        {
            piece = str.substr(start);
            start = std::string_view::npos;
        }
        else
        {
            piece = str.substr(start, end - start);
            start = end + 1;
        }

        result.emplace_back(piece);
    }

    return result;
}

// Parses the |numbers| vector representing the different numbers inside the version string and
// constructs a vector of valid integers. It stops when it reaches an invalid item (including the
// wildcard character). |parsed| is the resulting integer vector. Function returns true if all
// numbers were parsed successfully, false otherwise.
bool parseVersionNumbers(std::string_view version_str, std::vector<uint32_t>* parsed)
{
    std::vector<std::string_view> numbers = splitString(version_str, '.');
    if (numbers.empty())
        return false;

    for (auto it = numbers.begin(); it != numbers.end(); ++it)
    {
        std::string_view str = *it;

        if (str.empty())
            return false;

        if (startsWith(str, "+"))
            return false;

        unsigned int num;

        if (!stringToUint(str, &num))
            return false;

        // This throws out leading zeros for the first item only.
        if (it == numbers.begin() && numberToString(num) != str)
            return false;

        // StringToUint returns unsigned int but Version fields are uint32_t.
        static_assert(sizeof(uint32_t) == sizeof(unsigned int));

        parsed->push_back(num);
    }

    return true;
}

// Compares version components in |components1| with components in
// |components2|. Returns -1, 0 or 1 if |components1| is less than, equal to,
// or greater than |components2|, respectively.
int compareVersionComponents(const std::vector<uint32_t>& components1,
                             const std::vector<uint32_t>& components2)
{
    const std::size_t count = std::min(components1.size(), components2.size());

    for (std::size_t i = 0; i < count; ++i)
    {
        if (components1[i] > components2[i])
            return 1;

        if (components1[i] < components2[i])
            return -1;
    }

    if (components1.size() > components2.size())
    {
        for (std::size_t i = count; i < components1.size(); ++i)
        {
            if (components1[i] > 0)
                return 1;
        }
    }
    else if (components1.size() < components2.size())
    {
        for (std::size_t i = count; i < components2.size(); ++i)
        {
            if (components2[i] > 0)
                return -1;
        }
    }

    return 0;
}

}  // namespace

Version::Version() = default;

Version::Version(const Version& other) = default;
Version& Version::operator=(const Version& other) = default;

Version::Version(Version&& other) = default;
Version& Version::operator=(Version&& other) = default;

Version::Version(uint32_t major, uint32_t minor, uint32_t build, uint32_t revision)
{
    components_.push_back(major);
    components_.push_back(minor);
    components_.push_back(build);
    components_.push_back(revision);
}

Version::~Version() = default;

Version::Version(std::string_view version_str)
{
    std::vector<uint32_t> parsed;

    if (!parseVersionNumbers(version_str, &parsed))
        return;

    components_.swap(parsed);
}

Version::Version(const std::vector<uint32_t>& components)
    : components_(std::move(components))
{
    // Nothing
}

bool Version::isValid() const
{
    return (!components_.empty());
}

// static
bool Version::isValidWildcardString(std::string_view wildcard_string)
{
    std::string version_string(wildcard_string);
    if (endsWith(version_string, ".*"))
        version_string.resize(version_string.size() - 2);

    Version version(version_string);
    return version.isValid();
}

int Version::compareToWildcardString(std::string_view wildcard_string) const
{
    DCHECK(isValid());
    DCHECK(Version::isValidWildcardString(wildcard_string));

    // Default behavior if the string doesn't end with a wildcard.
    if (!endsWith(wildcard_string, ".*"))
    {
        Version version(wildcard_string);
        DCHECK(version.isValid());
        return compareTo(version);
    }

    std::vector<uint32_t> parsed;
    const bool success = parseVersionNumbers(
        wildcard_string.substr(0, wildcard_string.length() - 2), &parsed);
    DCHECK(success);
    const int comparison = compareVersionComponents(components_, parsed);
    // If the version is smaller than the wildcard version's |parsed| vector,
    // then the wildcard has no effect (e.g. comparing 1.2.3 and 1.3.*) and the
    // version is still smaller. Same logic for equality (e.g. comparing 1.2.2 to
    // 1.2.2.* is 0 regardless of the wildcard). Under this logic,
    // 1.2.0.0.0.0 compared to 1.2.* is 0.
    if (comparison == -1 || comparison == 0)
        return comparison;

    // Catch the case where the digits of |parsed| are found in |components_|,
    // which means that the two are equal since |parsed| has a trailing "*".
    // (e.g. 1.2.3 vs. 1.2.* will return 0). All other cases return 1 since
    // components is greater (e.g. 3.2.3 vs 1.*).
    DCHECK_GT(parsed.size(), 0UL);

    const std::size_t min_num_comp = std::min(components_.size(), parsed.size());
    for (std::size_t i = 0; i < min_num_comp; ++i)
    {
        if (components_[i] != parsed[i])
            return 1;
    }
    return 0;
}

int Version::compareTo(const Version& other) const
{
    DCHECK(isValid());
    DCHECK(other.isValid());

    return compareVersionComponents(components_, other.components_);
}

const std::string Version::toString() const
{
    DCHECK(isValid());

    std::string version_str;

    const std::size_t count = components_.size() - 1;
    for (std::size_t i = 0; i < count; ++i)
    {
        version_str.append(numberToString(components_[i]));
        version_str.append(".");
    }

    version_str.append(numberToString(components_[count]));
    return version_str;
}

bool operator==(const Version& v1, const Version& v2)
{
    return v1.compareTo(v2) == 0;
}

bool operator!=(const Version& v1, const Version& v2)
{
    return !(v1 == v2);
}

bool operator<(const Version& v1, const Version& v2)
{
    return v1.compareTo(v2) < 0;
}

bool operator<=(const Version& v1, const Version& v2)
{
    return v1.compareTo(v2) <= 0;
}

bool operator>(const Version& v1, const Version& v2)
{
    return v1.compareTo(v2) > 0;
}

bool operator>=(const Version& v1, const Version& v2)
{
    return v1.compareTo(v2) >= 0;
}

std::ostream& operator<<(std::ostream& stream, const Version& v)
{
    return stream << v.toString();
}

} // namespace base
