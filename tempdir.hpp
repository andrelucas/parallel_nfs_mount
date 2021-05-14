/**
 * @file tempdir.hpp
 * @author André Lucas (andre.lucas@storageos.com)
 * @brief RAII temporary directory support.
 * @version 0.1
 * @date 2019-11-01
 *
 * @copyright Copyright (c) 2019
 *
 */

#pragma once

#include <filesystem>
#include <string>

namespace fs = std::filesystem;

#include "limits.h"

/****************************************************************************/

/**
 * @brief RAII temporary directory.
 *
 * Intended for unit tests, where I get very tired of the ritual of setting up
 * and tearing down temp dirs.
 *
 * Creates a temp dir starting with std::filesystem::temp_directory_path()
 * and with an optional prefix (defaulting to "temp") followed by ".XXXXXX".
 * The 'X's are replaced by mkdtemp(3) with random contents, so the temp
 * directory name isn't predictable. Linux only changes six 'X' characters
 * (and in fact insists that the path ends in six 'X's) so we'll go with that.
 *
 * The directory will be deleted on object destruction, unless
 * PreserveContents() is called during the object's lifetime, or if the
 * environment variable PRESERVE_TMP is set to a true value.
 *
 * Usage should be no more complex than:
 *
 * ```
 * class FooUnit : public ::testing::Test {
 * public:
 *   TemporaryDirectory tmp;
 * };
 * ```
 *
 * Then, as each case runs, the directory is created, is queriable via
 * tmp->Dir(), and will be deleted at the end of the case. Naturally, `tmp`
 * can be in a standalone (non-fixture) test as a temporary variable too. As
 * long as its destructor gets called automatically, it's fine.
 */
class TemporaryDirectory {
   public:
    /**
     * @brief Construct a new temporary directory with prefix "temp".
     */
    TemporaryDirectory() { Create("temp"); }
    /**
     * @brief Construct a new Temporary Directory object with the provided
     * prefix.
     *
     * @param prefix A string prefix placed before '.XXXXXX', to form the
     * template passed to mkdtemp(3).
     */
    TemporaryDirectory(std::string prefix) { Create(prefix); }
    ~TemporaryDirectory() { DeleteNow(); }

    //! Return the temporary directory path.
    fs::path Dir() { return dir_; }

    void DeleteNow() {
        // Allow the user to prevent deletion.
        // bool skipdelete;
        // if (env_get_bool("PRESERVE_TMP", &skipdelete) && skipdelete)
        //     return;
        if (!preserve_ && dir_ != "")
            fs::remove_all(dir_);
    }
    //! Prevent deletion of the directory at object destruction time.
    void PreserveContents() { preserve_ = true; }
    //! Allow the deletion of the directory at object destruction time. This
    //! is the default behaviour.
    void DiscardContents() { preserve_ = false; }

   private:
    void Create(std::string prefix) {
        char tmp[PATH_MAX];  // mkdtemp() wants a mutable C string.
        auto tmpdir = fs::temp_directory_path();
        tmpdir /= prefix + ".XXXXXX";
        strncpy(tmp, tmpdir.c_str(), sizeof(tmp));
        if (mkdtemp(tmp) != tmp) {
            throw std::runtime_error("Failed to create tmp dir");
        }
        // tmp has been changed, we can't use tmpdir..
        dir_ = fs::path{tmp};
    }

    fs::path dir_;
    bool preserve_ = false;
};  // class TemporaryDirectory
