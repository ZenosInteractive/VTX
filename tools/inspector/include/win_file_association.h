#pragma once

#include <string>

// Per-user (.vtx) shell file-association setup for VTX Inspector.
//
// "Open with..." and double-click only hand the app a file path (as argv[1]) when a
// file association with a "%1" command is registered. These helpers write/remove that
// association under HKCU (no administrator rights). On non-Windows platforms they are
// no-ops that report as much -- there is no shell association to write.
namespace VtxInspector {

    struct AssociationResult {
        bool ok = false;
        std::string message; // human-readable detail (UTF-8/ASCII)
    };

    struct EnsureResult {
        bool changed = false; // true when startup auto-setup actually wrote keys
        std::string message;
    };

    // Startup auto-setup. Ensures this executable is registered as the .vtx handler and
    // listed under the extension's "Open with", so double-click and "Open with" work
    // without the user running --register. Idempotent and cheap: rewrites only when the
    // stored command does not already point at this exe (so it also self-heals after the
    // build is moved or rebuilt). Never steals the .vtx default from another app -- it
    // claims the default only when unset or already ours -- and does nothing when the
    // user turned the association off with --unregister.
    EnsureResult EnsureVtxAssociationOnStartup();

    // Explicit registration (--register): like the startup path but always claims the
    // .vtx default and re-enables auto-setup if it was turned off. Points at the running
    // executable, so it associates exactly this build.
    AssociationResult RegisterVtxAssociation();

    // Explicit removal (--unregister): removes what we wrote (best effort; only clears the
    // .vtx default when it still points at our ProgID) and disables the startup auto-setup
    // so a later launch does not silently re-add it.
    AssociationResult UnregisterVtxAssociation();

} // namespace VtxInspector
