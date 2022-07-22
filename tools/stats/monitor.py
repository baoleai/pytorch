#!/usr/bin/env python3
import datetime
import json
import subprocess
import sys
import time
from typing import Any, Dict, List


def pip_install(package_name: str) -> None:
    package_exists = subprocess.run(
        [sys.executable, "-m", package_name, "--help"], capture_output=True
    )
    if package_exists.returncode != 0:
        output = subprocess.run(
            [sys.executable, "-m", "pip", "install", package_name], capture_output=True
        )
        if output.returncode != 0:
            print(output.stderr.decode("utf-8"))
            print(output.stdout.decode("utf-8"))
    # when running this script in the background, pip installs the packages to
    # a location that is not in a path python uses to look for modules, so we
    # manually add the location to the path
    location = subprocess.run(
        [sys.executable, "-m", "pip", "show", package_name],
        capture_output=True,
    )
    for s in location.stdout.decode("utf-8").splitlines():
        if "Location:" in s:
            sys.path.append(s.split(" ")[1])
            print(location.stdout.decode("utf-8"))


def main() -> None:
    print(sys.path)
    pip_install("psutil")
    pip_install("pynvml")
    print(sys.path)
    import psutil  # type: ignore[import]
    import pynvml  # type: ignore[import]

    def get_processes_running_python_tests() -> List[Any]:
        python_processes = []
        for process in psutil.process_iter():
            try:
                if "python" in process.name() and process.cmdline():
                    python_processes.append(process)
            except Exception:
                # access denied
                pass
        return python_processes

    def get_per_process_cpu_info() -> List[Dict[str, Any]]:
        processes = get_processes_running_python_tests()
        per_process_info = []
        for p in processes:
            info = {
                "pid": p.pid,
                "cmd": " ".join(p.cmdline()),
                "cpu_percent": p.cpu_percent(),
                "rss_memory": p.memory_info().rss,
                "uss_memory": p.memory_full_info().uss,
            }
            try:
                info["pss_memory"] = p.memory_full_info().pss
            except Exception:
                pass
            per_process_info.append(info)
        return per_process_info

    def get_per_process_gpu_info(handle: Any) -> List[Dict[str, Any]]:
        processes = pynvml.nvmlDeviceGetComputeRunningProcesses(handle)
        per_process_info = []
        for p in processes:
            info = {"pid": p.pid, "gpu_memory": p.usedGpuMemory}
            per_process_info.append(info)
        return per_process_info

    handle = None
    try:
        pynvml.nvmlInit()
        handle = pynvml.nvmlDeviceGetHandleByIndex(0)
    except Exception:
        pass

    while True:
        try:
            stats = {
                "time": datetime.datetime.utcnow().isoformat("T") + "Z",
                "total_cpu_percent": psutil.cpu_percent(),
                "per_process_cpu_info": get_per_process_cpu_info(),
            }
            if handle is not None:
                stats["per_process_gpu_info"] = get_per_process_gpu_info(handle)
        except Exception as e:
            stats = {
                "time": datetime.datetime.utcnow().isoformat("T") + "Z",
                "error": str(e),
            }
        finally:
            print(json.dumps(stats))
            time.sleep(1)


if __name__ == "__main__":
    main()
