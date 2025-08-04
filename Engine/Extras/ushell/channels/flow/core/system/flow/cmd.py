# Copyright Epic Games, Inc. All Rights Reserved.

import os
import sys
import marshal
from enum import Enum

from . import _backtrace
from ._noticeboard import Noticeboard
from .text import *

#-------------------------------------------------------------------------------
def _lovely_info(*message):
    with text.light_cyan:
        print("--", *message)

def _lovely_warning(*message):
    with text.light_yellow:
        print("!!", *message)

def _lovely_error(*message):
    with text.light_red:
        print("!!", *message)



#-------------------------------------------------------------------------------
from . import _runnable
class _Runnable(_runnable.Runnable):
    def launch(self, **kwargs):
        if "silent" not in kwargs:
            with text.cyan:
                print("== Run: ", end="")
            with text.white:
                it = iter(self.read_args())
                print(os.fspath(next(it, "")).replace("/", os.sep), end=" ")
                print(*it)
        else:
            del kwargs["silent"]

        return super().launch(**kwargs)

#-------------------------------------------------------------------------------
class _Env(object):
    def __init__(self):
        try:
            import nt
            self._original = nt.environ
            if "PROMPT" in self._original:
                del self._original["PROMPT"]
        except ModuleNotFoundError:
            import os
            self._original = os.environ
        self._env = self._original.copy()
        self._keys = {x.upper():x for x in self._env.keys()}
        self._edits = set()

    def __setitem__(self, key, value):
        key = self._map_key(key, True)
        self._env[key] = str(value)
        self._edits.add(key)

    def __getitem__(self, key):
        key = self._map_key(key, False)
        return self._env[key]

    def __delitem__(self, key):
        key = self._map_key(key, False)
        del self._env[key]
        del self._keys[key.upper()]

    def _map_key(self, key, add):
        ret = self._keys.get(key.upper(), None);
        if not ret and add:
            self._keys[key.upper()] = key
        return ret or key

    def update(self, rhs):
        for key, value in rhs.items():
            self[key] = value

    def read_changes(self):
        for key in self._edits:
            if key in self._env:
                yield key, self._env[key]

        for key, value in self._env.items():
            if value != self._original.get(key, None):
                yield key, value

        for key in self._original.keys():
            if key.upper() not in self._keys:
                yield key, None

    def __iter__(self): return iter(self.keys())
    def keys(self):     return self._env.keys()
    def values(self):   return self._env.values()
    def items(self):    return self._env.items()
    def get(self, *a):  return self._env.get(*a)

#-------------------------------------------------------------------------------
class _ExecContext(object):
    def __init__(self):
        self._env = None

    def get_env(self):
        self._env = self._env or _Env()
        return self._env

    def create_runnable(self, cmd, *args):
        return _Runnable(cmd, *args, env=self._env)



#-------------------------------------------------------------------------------
class _ArgOverrider(object):
    def _apply_arg_overrides(self):
        try:
            self._apply_guarded()
        except Exception as e:
            print("ArgOverrider:", "ERROR:", str(e))

    def _apply_guarded(self):
        if not self._invoke_path:
            return

        header = "Argument overrides"
        for arg_name, arg_value in self.args:
            if not isinstance(arg_value, tuple):
                if not self.args.is_default(arg_name):
                    continue

            over_value = os.getenv(f"ushell{self._invoke_path}:{arg_name}")
            if over_value is None:
                continue

            if header:
                self.print_info(header)
                header = None

            print(arg_name + ": ", end="")
            try:
                arg_type = self.args.get_type(arg_name)
                if isinstance(arg_value, tuple):
                    import shlex
                    over_value = (arg_type(x) for x in shlex.split(over_value))
                    over_value = (*arg_value, *over_value)
                    print(*over_value, end="")
                else:
                    over_value = arg_type(over_value)
                    print(over_value, end="")
            except Exception as e:
                print("[ERROR:", str(e))
            print(" (env)")

            setattr(self.args, arg_name, over_value)



#-------------------------------------------------------------------------------
from . import _flick
Arg = _flick.Arg
Opt = _flick.Opt
class Cmd(_flick.Cmd, _ArgOverrider):
    Arg = _flick.Arg
    Opt = _flick.Opt

    class Noticeboard(Enum):
        SESSION     = 0
        PERSISTENT  = 1

    def _print_error(self, message):
        with text.light_red:
            super()._print_error(message)

    def _print_help(self, *args, **kwargs):
        with text.light_cyan:
            super()._print_help(*args, **kwargs)

    def _call_main(self):
        # Some host shells such as Bash set $PWD to the current directory. p4
        # detects this and uses it instead of its current directory. This can
        # cause subtle bugs in Bash-hosted ushell sessions with patterns like
        # 'os.chdir(x); subprocess.run("p4 set")'. Unset PWD.
        if "PWD" in os.environ:
            del os.environ["PWD"]

        self._apply_arg_overrides()

        try:
            return super()._call_main()
        except KeyboardInterrupt as e:
            print()
            self.print_warning("Interrupted!", str(e))
            raise SystemExit(1)

    def post_construct(self):
        pass

    def is_interactive(self):
        if os.name != "nt":
            return sys.stdout.isatty() and sys.stdin.isatty()

        # Windows says "NUL" is a character device so MSVC's isatty() returns
        # true. flow.native.isatty() is more accurate.
        from . import native as flow_native
        return flow_native.isatty(0) and flow_native.isatty(1) # 0=in, 1=out

    def get_os_name(self):
        if os.name == "nt":          return "nt"
        if sys.platform == "darwin": return "mac"
        if sys.platform == "linux":  return "linux"
        return "unknown"

    def get_channel(self):
        return self._channel

    def get_home_dir(self):
        return os.path.expanduser("~/.ushell/")

    def get_noticeboard(self, board_type):
        temp_dir = self._channel.get_system().get_temp_dir()
        flow_sid = -1
        if board_type == Cmd.Noticeboard.SESSION:
            flow_sid = os.getenv("FLOW_SID", "x")
            flow_sid = int(flow_sid) if flow_sid.isdecimal() else "-493"
        return Noticeboard(temp_dir + "noticeboard", int(flow_sid))

    def edit_file(self, path):
        if editor := os.getenv("GIT_EDITOR") or os.getenv("P4EDITOR"):
            if not os.path.isfile(editor):
                import shlex
                exe, *args = (*shlex.split(editor), path)
            else:
                exe, *args = (editor, path)
        else:
            exe = "notepad.exe" if os.name == "nt" else "nano"
            args = (path,)

        import subprocess
        subprocess.run((exe, *args))

    def print_info(self, *message):
        _lovely_info(*message)

    def print_warning(self, *message):
        _lovely_warning(*message)

    def print_error(self, *message):
        _lovely_error(*message)

    def get_exec_context(self):
        return _ExecContext()

    @staticmethod
    def summarise(main_method):
        import time
        import datetime

        def finish(start_time, result):
            exec_time = time.time() - start_time

            if isinstance(result, bool):
                result = 0 if result is True else 1

            print("")
            if result: _lovely_info("Result: " + text.light_red("Failed - " + hex(result)))
            else: _lovely_info("Result: " + text.green("Success"))
            _lovely_info("  Time:", str(datetime.timedelta(seconds=int(exec_time))))

            return result

        def begin(*args, **kwargs):
            start_time = time.time()
            result = main_method(*args, **kwargs)
            if getattr(result, "cr_code", None):
                async def inner(result):
                    result = await result
                    return finish(start_time, result)
                return inner(result)

            return finish(start_time, result)
        return begin
