-- =============================================================================
-- GeNESis-APU2PSG -- load this in FCEUX or Gens when a script says it cannot
-- find nes_apu_data.txt.
--
-- It exists because "not found" is what the *script* said, not what the OS
-- said. io.open returns an error string as its second value, and every version
-- of these scripts discarded it -- so a permission error, a locked file, a
-- sandboxed io library and a genuinely absent file all printed the same line.
-- This prints the real reason, for every path worth trying.
-- =============================================================================

local BASENAME = "nes_apu_data.txt"

local function scriptDir()
    if not debug or not debug.getinfo then return nil end
    local info = debug.getinfo(1, "S")
    if not info or not info.source then return nil end
    local src = info.source
    if string.sub(src, 1, 1) == "@" then src = string.sub(src, 2) end
    return string.match(src, "^(.*[/\\])")
end

print("=== GeNESis-APU2PSG file diagnostic ===")

-- 1. Is the io library even there? Some emulator Lua sandboxes drop it.
if type(io) ~= "table" or type(io.open) ~= "function" then
    print("FATAL: this emulator's Lua has no usable io library.")
    print("       io = " .. type(io))
    return
end
print("io library      : present")
print("script directory: " .. tostring(scriptDir() or "unknown (no debug library)"))

-- 2. Where does a bare relative path actually land? Write a probe and see if a
--    known-good open of it succeeds; that is the emulator's working directory.
local probe, perr = io.open("gapu2_probe.tmp", "w")
if probe then
    probe:write("probe\n")
    probe:close()
    print("write test      : OK -- a bare relative path is writable from here")
    os.remove("gapu2_probe.tmp")
else
    print("write test      : FAILED -- " .. tostring(perr))
    print("                  the working directory is read-only or sandboxed")
end

-- 3. Try every candidate and report the OS's actual reason for each.
local candidates = {
    (scriptDir() or "") .. BASENAME,
    BASENAME,
    "./" .. BASENAME,
    "../" .. BASENAME,
}

print("")
print("candidate paths:")
local found = false
for _, path in ipairs(candidates) do
    local f, err = io.open(path, "r")
    if f then
        local size = f:seek("end")
        f:seek("set")
        local first = f:read("*l")
        f:close()
        found = true
        print(string.format("  OPENED  %s", path))
        print(string.format("          %d bytes, first line: %s",
              size, tostring(first and string.sub(first, 1, 60) or "(empty)")))
        if size == 0 then
            print("          WARNING: file is empty. The recorder creates it on load")
            print("                   and fills it as the NES runs -- let it play.")
        end
    else
        print(string.format("  failed  %-44s %s", path, tostring(err)))
    end
end

print("")
if found then
    print("At least one path opened. Point the player at that exact path.")
else
    print("Nothing opened. Read the reasons above:")
    print("  'No such file or directory' -> the name or folder is wrong. On Windows,")
    print("     check for a hidden second extension (nes_apu_data.txt.txt).")
    print("  'Permission denied' -> the folder is protected (Program Files), or the")
    print("     recorder still holds the file open. Close FCEUX and retry.")
    print("  anything else -> that string is the real bug; report it.")
end
print("=== end diagnostic ===")
