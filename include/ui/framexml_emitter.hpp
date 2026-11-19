#pragma once

// Turns FrameXML into the Lua that builds it.
//
// The alternative was to build widgets from C++ directly, walking the XML and
// calling into the tree. That would have meant a second implementation of
// everything CreateFrame already does - parenting, naming, templates, script
// binding - kept in step with the first by hand. Emitting Lua instead means XML
// frames and hand-written frames travel the same path, so anything fixed for
// one is fixed for both, and a template defined in XML is usable from a script
// without translation.
//
// Emitting source rather than driving Lua directly also makes this testable
// without a Lua state: the output is a string, and a test can read it.

#include <string>
#include <vector>

namespace wowee {
namespace ui {

struct XmlNode;

struct EmitResult {
    std::string lua;
    /// Files named by <Script file="..."/> and <Include file="..."/>, in the
    /// order they appeared. `lua` calls __WoweeXmlStep(i) (1-based) where
    /// file i stood among the frames and inline scripts, so the caller loads
    /// it at that point - build 8606 reads a document in order, and Bagnon's
    /// embeds.xml loads LibStub with a Script before the Includes that use it.
    /// Resolving paths is the caller's job, not the emitter's.
    struct FileStep {
        std::string file;
        bool include = false;
    };
    std::vector<FileStep> files;
    std::vector<std::string> warnings;
};

/// Emit Lua for a parsed <Ui> document. `virtualPrefix` names the table that
/// virtual (template) frames are recorded in so later `inherits` can find them.
EmitResult emitFrameXml(const XmlNode& root);

/// Substitute FrameXML's $parent token. A child named "$parentText" inside
/// "FooFrame" becomes "FooFrameText"; this is how nearly every region in the
/// original interface is named.
std::string substituteParent(const std::string& name, const std::string& parentName);

} // namespace ui
} // namespace wowee
