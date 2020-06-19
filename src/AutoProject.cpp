#include <config.h>
#include "AutoProject.h"
#include <unordered_set>
#include <algorithm>
#include <iostream>
#include <regex>
#include <string_view>

using namespace std::literals;

// local constants
static const std::string mdextension{".md"};
static constexpr std::string_view cmakeVersion{"VERSION 3.1"};
static constexpr unsigned indentLevel{4};
static constexpr unsigned delimLength{3};

// helper functions
static std::string& trim(std::string& str, const std::string_view pattern);
static std::string& rtrim(std::string& str, const std::string_view pattern);
static std::string& trim(std::string& str, char ch);
static std::string& rtrim(std::string& str, char ch);
static std::string trimExtras(std::string& line);
static bool isNonEmptyIndented(const std::string& line);
static bool isIndentedOrEmpty(const std::string& line);
static bool isEmptyOrUnderline(const std::string& line);
static bool isDelimited(const std::string& line);
static bool isSourceExtension(const std::string_view ext);
static bool isSourceFilename(std::string &line);
static std::string &replaceLeadingTabs(std::string &line);
static void emit(std::ostream& out, const std::string &line);
static void emitVerbatim(std::ostream& out, const std::string &line);

void AutoProject::open(fs::path mdFilename)  {
    AutoProject ap(mdFilename);
    std::swap(ap, *this);
}

AutoProject::AutoProject(fs::path mdFilename) :
    mdfile{mdFilename},
    projname{mdfile.stem().string()},
    srcdir{projname + "/src"},
    in(mdfile)
{
    if (mdfile.extension() != mdextension) {
        throw FileExtensionException("Input file must have " + mdextension + " extension");
    }
    if (!in) {
        throw std::runtime_error("Cannot open input file "s + mdfile.string());
    }
}

/// returns true if passed file extension is an identified source code extension.
bool isSourceExtension(const std::string_view ext) {
    static const std::unordered_set<std::string_view> source_extensions{".cpp", ".c", ".h", ".hpp"};
    return source_extensions.find(ext) != source_extensions.end();
}

/*
 * As of January 2019, according to this post:
 * https://meta.stackexchange.com/questions/125148/implement-style-fenced-markdown-code-blocks
 * an alternative of using either ```c++ or ~~~lang-c++ with a matching end
 * tag and unindented code is now supported in addition to the original
 * indented flavor.  As a result, this code is modified to also accept
 * that syntax as of April 2019.
 */
bool AutoProject::createProject(bool overwrite) {
    std::string prevline;
    bool inIndentedFile{false};
    bool inDelimitedFile{false};
    bool firstFile{true};
    std::ofstream srcfile;
    fs::path srcfilename;
    // TODO: this might be much cleaner with a state machine
    for (std::string line; getline(in, line); ) {
        replaceLeadingTabs(line);
        // scan through looking for lines indented with indentLevel spaces
        if (inIndentedFile) {
            // stop writing if non-indented line or EOF
            if (!isIndentedOrEmpty(line)) {
                std::swap(prevline, line);
                srcfile.close();
                inIndentedFile = false;
            } else {
                checkRules(line);
                emit(srcfile, line);
            }
        } else if (inDelimitedFile) {
            // stop writing if delimited line
            if (isDelimited(line)) {
                std::swap(prevline, line);
                srcfile.close();
                inDelimitedFile = false;
            } else {
                checkRules(line);
                emitVerbatim(srcfile, line);
            }
        } else {
            if (isDelimited(line)) {
                // if previous line was filename, open that file and start writing
                if (isSourceFilename(prevline)) {
                    srcfilename = fs::path(srcdir) / prevline;
                } else {
                    srcfilename = fs::path(srcdir) / "main.cpp";
                }
                if (firstFile) {
                    makeTree(overwrite);
                    firstFile = false;
                }
                srcfile.open(srcfilename);
                if (srcfile) {
                    srcnames.emplace(srcfilename.filename());
                    inDelimitedFile = true;
                }
            } else if (isNonEmptyIndented(line)) {
                // if previous line was filename, open that file and start writing
                if (isSourceFilename(prevline)) {
                    if (firstFile) {
                        makeTree(overwrite);
                        firstFile = false;
                    }
                    srcfilename = fs::path(srcdir) / prevline;
                    srcfile.open(srcfilename);
                    if (srcfile) {
                        checkRules(line);
                        emit(srcfile, line);
                        srcnames.emplace(srcfilename.filename());
                        inIndentedFile = true;
                    }
                } else if (firstFile && !line.empty()) {  // un-named source file
                    makeTree(overwrite);
                    firstFile = false;
                    srcfilename = fs::path(srcdir) / "main.cpp";
                    srcfile.open(srcfilename);
                    if (srcfile) {
                        checkRules(line);
                        emit(srcfile, line);
                        srcnames.emplace(srcfilename.filename());
                        inIndentedFile = true;
                    }
                }
            } else {
                if (!isEmptyOrUnderline(line))
                    std::swap(prevline, line);
            }
        }
    }
    in.close();
    if (!srcnames.empty()) {
        writeSrcLevel();
        writeTopLevel();
        // copy md file to projname/src
        fs::copy_file(mdfile, srcdir + "/" + projname + mdextension);
    }
    return !srcnames.empty();
}

void AutoProject::makeTree(bool overwrite) {
    if (fs::exists(projname) && !overwrite) {
        throw std::runtime_error(projname + " already exists: will not overwrite.");
    }
    if (!fs::create_directories(srcdir)) {
        throw std::runtime_error("Cannot create directory "s + srcdir);
    }
    fs::create_directories(projname + "/build");
}

std::string& trim(std::string& str, const std::string_view pattern) {
    // TODO: when we get C++20, use std::string::starts_with()
    if (str.find(pattern) == 0) {
        str.erase(0, pattern.size());
    }
    return str;
}

std::string& rtrim(std::string& str, const std::string_view pattern) {
    // TODO: when we get C++20, use std::string::ends_with()
    auto loc{str.rfind(pattern)};
    if (loc != std::string::npos && loc == str.size() - pattern.size()) {
        str.erase(str.size() - pattern.size(), pattern.size());
    }
    return str;
}

std::string& trim(std::string& str, char ch) {
    auto it{str.begin()};
    for ( ; (*it == ch || isspace(*it)) && it != str.end(); ++it)
    { }
    if (it != str.end()) {
        str.erase(str.begin(), it);
    }
    return str;
}

std::string& rtrim(std::string& str, char ch) {
    std::reverse(str.begin(), str.end());
    trim(str, ch);
    std::reverse(str.begin(), str.end());
    return str;
}

bool isSourceFilename(std::string &line) {
    trimExtras(line);
    return isSourceExtension(fs::path(line).extension().string());
}

std::string trimExtras(std::string& line) {
    if (line.empty()) {
        return line;
    }
    // remove header markup
    trim(line, '#');
    rtrim(line, '#');
    // remove bold or italic
    trim(line, '*');
    rtrim(line, '*');
    // remove html bold
    trim(line, "<b>");
    rtrim(line, "</b>");
    // remove quotes
    trim(line, '"');
    rtrim(line, '"');
    // remove trailing - or :
    rtrim(line, '-');
    rtrim(line, ':');
    return line;
}

void AutoProject::writeSrcLevel() const {
    // write CMakeLists.txt with filenames to projname/src
    std::ofstream srccmake(srcdir + "/CMakeLists.txt");
    // TODO: the add_executable line needs to be *after* "set(CMAKE_AUTOMOC ON)" but before "target_link_libraries..."
    srccmake <<
            "cmake_minimum_required(" << cmakeVersion << ")\n"
            "set(EXECUTABLE_NAME \"" << projname << "\")\n";
    for (const auto &rule : extraRules) {
        srccmake << rule << '\n';
    }
    srccmake <<
            "add_executable(${EXECUTABLE_NAME}";
    for (const auto& fn : srcnames) {
        srccmake << ' ' << fn;
    }
    srccmake <<
            ")\ntarget_link_libraries(${EXECUTABLE_NAME} ";
    for (const auto &lib : libraries) {
        srccmake << lib << ' ';
    }
    srccmake << ")\n";
    srccmake.close();
}

void AutoProject::writeTopLevel() const {
    // TODO: use replaceable boilerplate in a config file
    // write CMakeLists.txt top level to projname
    std::ofstream topcmake(projname + "/CMakeLists.txt");
    topcmake <<
            "cmake_minimum_required(" << cmakeVersion << ")\n"
            "project(" << projname << ")\n"
            "set(CMAKE_CXX_STANDARD 14)\n"
            "set(CMAKE_CXX_FLAGS \"${CMAKE_CXX_FLAGS} -Wall -Wextra -pedantic\")\n"
            "add_subdirectory(src)\n";
}

void AutoProject::checkRules(const std::string &line) {
    // TODO: provide mechanism to load rules from file(s)
    static const struct Rule {
        const std::regex re;
        const std::string cmake;
        const std::string libraries;
        Rule(std::string reg, std::string result, std::string libraries) : re{reg}, cmake{result}, libraries{libraries} {}
    } rules[]{
        { R"(\s*#include\s*<(experimental/)?filesystem>)","",
            "stdc++fs" },
        { R"(\s*#include\s*<thread>)", "find_package(Threads REQUIRED)\n",
                "${CMAKE_THREAD_LIBS_INIT}"},
        { R"(\s*#include\s*<future>)", "find_package(Threads REQUIRED)\n",
                "${CMAKE_THREAD_LIBS_INIT}"},
        { R"(\s*#include\s*<SFML/Graphics.hpp>)",
                    "find_package(SFML REQUIRED COMPONENTS System Window Graphics)\n"
                    "include_directories(${SFML_INCLUDE_DIR})\n",
                    "${SFML_LIBRARIES}"},
        { R"(\s*#include\s*<GL/glew.h>)",
                    "find_package(GLEW REQUIRED)\n",
                    "${GLEW_LIBRARIES}" },
        { R"(\s*#include\s*<GL/glut.h>)",
                    "find_package(GLUT REQUIRED)\n"
                    "find_package(OpenGL REQUIRED)\n",
                    "${OPENGL_LIBRARIES} ${GLUT_LIBRARIES}" },
        { R"(\s*#include\s*<OpenGL/gl.h>)",
                    "find_package(OpenGL REQUIRED)\n",
                    "${OPENGL_LIBRARIES}" },
        { R"(\s*#include\s*<SDL2/SDL.h>)",
                    "find_package(SDL2 REQUIRED)\n",
                    "${SDL2_LIBRARIES}" },
        // the SDL2_ttf.cmake package doesn't yet ship with CMake
        { R"(\s*#include\s*<SDL2/SDL_ttf.h>)",
                    "find_package(SDL2_ttf REQUIRED)\n",
                    "${SDL2_TTF_LIBRARIES}" },
        { R"(\s*#include\s*<GLFW/glfw3.h>)",
                    "find_package(glfw3 REQUIRED)\n",
                    "glfw" },
        { R"(\s*#include\s*<boost/regex.hpp>)",
                    "find_package(Boost REQUIRED COMPONENTS regex)\n",
                    "${Boost_LIBRARIES}" },
        { R"(\s*#include\s*<png.h>)",
                    "find_package(PNG REQUIRED)\n",
                    "${PNG_LIBRARIES}" },
        { R"(\s*#include\s*<ncurses.h>)",
                    "find_package(Curses REQUIRED)\n",
                    "${CURSES_LIBRARIES}" },
        { R"(\s*#include\s*<SDL2.SDL.h>)",
            R"(include(FindPkgConfig)
PKG_SEARCH_MODULE(SDL2 REQUIRED sdl2)
INCLUDE_DIRECTORIES(${SDL2_INCLUDE_DIRS})
)",
                    "${SDL2_LIBRARIES}" },
        // experimental support for Qt5; not sure if Widgets is correct
        { R"(\s*#include\s*<QString>)",
                    R"(find_package(Qt5Widgets)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_INCLUDE_CURRENT_DIR ON)
)",
                    "Qt5::Widgets"},
        { R"(\s*#include\s*<openssl/ssl.h>)",
                    "find_package(OpenSSL REQUIRED)\n",
                    "${OPENSSL_LIBRARIES}" },
    };
    std::smatch sm;
    for (const auto &rule : rules) {
        if (std::regex_search(line, sm, rule.re)) {
            extraRules.emplace(rule.cmake);
            libraries.emplace(rule.libraries);
        }
    }
}

bool isNonEmptyIndented(const std::string& line) {
    size_t indent{line.find_first_not_of(' ')};
    return indent >= indentLevel && indent != std::string::npos;
}

bool isIndentedOrEmpty(const std::string& line) {
    size_t indent{line.find_first_not_of(' ')};
    return indent >= indentLevel;
}

bool isEmptyOrUnderline(const std::string& line) {
    size_t indent{line.find_first_not_of('-')};
    return line.empty() || indent == std::string::npos;
}

bool isDelimited(const std::string& line) {
    if (line.empty() || (line[0] != '`' && line[0] != '~')) {
        return false;
    }
    size_t backtickDelim{line.find_first_not_of('`')};
    size_t tildeDelim{line.find_first_not_of('~')};
    return backtickDelim >= delimLength || tildeDelim >= delimLength;
}

std::string &replaceLeadingTabs(std::string &line) {
    std::size_t tabcount{0};
    for (auto ch: line) {
        if (ch != '\t')
            break;
        ++tabcount;
    }
    if (tabcount) {
        line.replace(0, tabcount, indentLevel*tabcount, ' ');
    }
    return line;
}

static void emit(std::ostream& out, const std::string &line) {
    if (line.size() < indentLevel) {
        out << line << '\n';
    } else {
        out << (line[0] == ' ' ? line.substr(indentLevel) : line.substr(1)) << '\n';
    }
}

void emitVerbatim(std::ostream& out, const std::string &line) {
    out << line << '\n';
}

std::ostream& operator<<(std::ostream& out, const AutoProject &ap) {
    out << "Successfully extracted the following source files:\n";
    std::copy(ap.srcnames.begin(), ap.srcnames.end(), std::ostream_iterator<fs::path>(out, "\n"));
    return out;
}
