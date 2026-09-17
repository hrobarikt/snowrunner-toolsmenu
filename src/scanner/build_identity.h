// Which executable the report is about.
//
// The diagnostics report exists so that a user on a build the author does not
// own can be helped from a paste. Signature RVAs alone cannot say which build
// they came from, so the report carries the file's version and a hash of it as
// well. See docs/design.md, "Product shape".
//
// Separate from the scanner proper, which is pure computation over a byte
// buffer: this one opens files.

#pragma once

#include <string>

namespace srtm {

struct BuildIdentity {
    std::string file;     // the path the bytes came from, as shown to a user
    std::string version;  // the fixed four-part file version, e.g. 1.0.0.0
    // The ProductVersion string, which is where SnowRunner puts the build
    // anyone actually names: 1.886173.SNOW_DLC_18. Its FileVersion is 1.0.0.0
    // on every build, so the four-part number alone identifies nothing.
    std::string build;
    std::string sha256;   // lowercase hex, empty if the file could not be read
};

// Reads `path` and describes it. Never throws and never fails loudly: a field
// that could not be determined comes back empty, because a report with three
// of four facts in it is still worth having.
BuildIdentity IdentifyFile(const std::wstring& path);

// The path of the running executable. What the injected module identifies.
std::wstring MainModulePath();

}  // namespace srtm
