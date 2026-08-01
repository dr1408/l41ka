"""Load PongoOS DWARF symbols into LLDB's selected target."""

from pathlib import Path

import lldb


REPO_ROOT = Path(__file__).resolve().parent.parent
DEFAULT_BUILD_DIRS = (
    REPO_ROOT / "cmake-build-debug/payloads/PongoOS",
    REPO_ROOT / "cmake-build-release/payloads/PongoOS",
    REPO_ROOT / "src/payloads/PongoOS/build",
)


def _find_build_dir(command: str) -> Path:
    if command.strip():
        build_dir = Path(command.strip()).expanduser().resolve()
        if build_dir.name == "Pongo.dSYM":
            build_dir = build_dir.parent
        elif build_dir.name == "Pongo":
            build_dir = build_dir.parent
        return build_dir

    for build_dir in DEFAULT_BUILD_DIRS:
        if (build_dir / "Pongo").is_file() and (build_dir / "Pongo.dSYM").is_dir():
            return build_dir

    raise FileNotFoundError("no Pongo build with a Pongo.dSYM was found")


def load_pongo_symbols(debugger, command, result, _internal_dict):
    """Load Pongo and its dSYM into the currently selected LLDB target."""
    try:
        build_dir = _find_build_dir(command)
        pongo = build_dir / "Pongo"
        dwarf = build_dir / "Pongo.dSYM/Contents/Resources/DWARF/Pongo"
        if not pongo.is_file():
            raise FileNotFoundError(f"Pongo executable not found at {pongo}")
        if not dwarf.is_file():
            raise FileNotFoundError(f"Pongo DWARF file not found at {dwarf}")

        target = debugger.GetSelectedTarget()
        if not target.IsValid():
            raise RuntimeError("LLDB has no selected target")

        for module in target.module_iter():
            module_path = module.GetFileSpec().fullpath
            if module_path and Path(module_path).name == "Pongo":
                target.RemoveModule(module)

        module_spec = lldb.SBModuleSpec()
        module_spec.SetFileSpec(lldb.SBFileSpec(str(pongo)))
        module_spec.SetSymbolFileSpec(lldb.SBFileSpec(str(dwarf)))
        module = target.AddModule(module_spec)
        if not module.IsValid():
            raise RuntimeError(f"LLDB could not add {pongo}")

        error = target.SetModuleLoadAddress(module, 0)
        if error.Fail():
            raise RuntimeError(error.GetCString())

        result.AppendMessage(f"Loaded Pongo symbols from {dwarf}")
    except Exception as error:
        result.SetError(str(error))


def __lldb_init_module(debugger, _internal_dict):
    debugger.HandleCommand(
        f"command script add -f {__name__}.load_pongo_symbols pongo-load-symbols"
    )
    debugger.HandleCommand("pongo-load-symbols")
