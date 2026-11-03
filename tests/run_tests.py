"""Host checks. Optional native libraries are inspected and patched, never run."""
from pathlib import Path
import argparse
import os
import re
import shlex
import shutil
import subprocess
import tempfile

ROOT=Path(__file__).resolve().parents[1]


def block(text,start):
    opening=text.index("{",start)
    end,depth=opening+1,1
    while depth:
        depth+=(text[end]=="{")-(text[end]=="}");end+=1
    return text[start:end]


def function(text,name):
    match=re.search(r"static\s+(?:void|int)\s+"+name+r"\(",text)
    if not match:raise ValueError(name)
    return block(text,match.start())


def main():
    parser=argparse.ArgumentParser()
    parser.add_argument("--library",action="append",nargs=2,metavar=("SO_PATH","PACKAGE"),default=[])
    args=parser.parse_args()
    compiler=shlex.split(os.environ["CC"]) if "CC" in os.environ else [
        next((p for name in ("cc","clang","gcc") if (p:=shutil.which(name))),"cc")]
    main_source=(ROOT/"source/main.c").read_text(encoding="utf-8")
    with tempfile.TemporaryDirectory(prefix="gdash-checks-",ignore_cleanup_errors=True) as temp:
        work=Path(temp)
        def build(name,sources,extra=()):
            exe=work/(name+(".exe" if os.name=="nt" else ""))
            subprocess.run(compiler+["-std=gnu11","-O2","-Wall","-Wextra","-Werror",
                "-I"+str(ROOT/"source"),"-I"+str(ROOT/"tests")]+list(extra)+
                [str(p) for p in sources]+["-o",str(exe)],check=True)
            return exe
        template=(ROOT/"tests/test_input.c").read_text(encoding="utf-8")
        code=function(main_source,"cursor_visible")+"\n"+function(main_source,"build_virtual_pointers")
        generated=work/"input.c";generated.write_text(template.replace("/* PRODUCTION_INPUT */",code))
        subprocess.run([str(build("input",[generated]))],check=True)
        thread_source=(ROOT/"source/pthr.c").read_text(encoding="utf-8")
        code=function(thread_source,"attr_static_init")+"\n"
        code+=thread_source[thread_source.index("// libnx provides joinable pthreads"):
                            thread_source.index("pthread_t pthread_self_soloader")]
        for name in ("pthread_attr_init_soloader","pthread_attr_destroy_soloader","pthread_attr_setdetachstate_soloader"):
            code+="\n"+block(thread_source,thread_source.index("int "+name+"("))
        template=(ROOT/"tests/test_pthr.c").read_text(encoding="utf-8")
        generated=work/"threads.c";generated.write_text(template.replace("/* PRODUCTION_THREADS */",code))
        subprocess.run([str(build("threads",[generated],["-pthread","-Wno-unused-function"]))],check=True)
        template=(ROOT/"tests/test_lifecycle.c").read_text(encoding="utf-8")
        code="\n".join(function(main_source,name) for name in
                       ("menu_save_state","save_game_state","applet_focus_hook","save_lifecycle_init"))
        loop=block(main_source,main_source.index("while (appletMainLoop() && !g_quit)"))
        body=loop[loop.index("{")+1:-1]
        generated=work/"lifecycle.c"
        generated.write_text(template.replace("/* PRODUCTION_LIFECYCLE */",code).replace("/* PRODUCTION_RENDER_LOOP */",body))
        save_work=work/"lifecycle";save_work.mkdir();(save_work/"save").mkdir()
        subprocess.run([str(build("lifecycle",[generated,ROOT/"source/save_manager.c"])),str(save_work)],check=True)
        save_work=work/"scheduled";save_work.mkdir();(save_work/"save").mkdir()
        subprocess.run([str(build("saves",[ROOT/"tests/test_save_manager.c",ROOT/"source/save_manager.c"])),str(save_work)],check=True)
        subprocess.run([str(build("config",[ROOT/"tests/test_config.c",ROOT/"source/config.c"],
                                  ["-I"+str(ROOT/"tests/platform")])),str(work/"config.txt")],check=True)
        exe=build("jni",[ROOT/"tests/test_jni.c",ROOT/"source/jni_fake.c"],
                  ["-I"+str(ROOT/"tests/platform"),"-D__UTIL_H__","-Wno-unused-parameter",
                   "-include",str(ROOT/"tests/compat_platform.h")])
        subprocess.run([str(exe)],check=True)
        if args.library:
            exe=build("compat",[ROOT/"tests/test_compat.c",ROOT/"source/game_compat.c"],
                      ["-include",str(ROOT/"tests/compat_platform.h")])
            for library,package in args.library:
                subprocess.run([str(exe),str(Path(library).resolve()),package],check=True)


if __name__=="__main__":main()
