# Copyright Epic Games, Inc. All Rights Reserved.

import os
import p4utils
import flow.cmd
import subprocess
from pathlib import Path

#-------------------------------------------------------------------------------
def _check_for_existing_nt(client):
    client = client.encode()

    wmic = ("tasklist.exe", "/v", "/nh", "/fo:csv", "/fi", "imagename eq p4v.exe")
    proc = subprocess.Popen(wmic, stdout=subprocess.PIPE)
    for line in iter(proc.stdout.readline, b""):
        if b"p4v.exe" in line and client in line:
            ret = True
            break
    else:
        ret = False
    proc.stdout.close()
    proc.wait()
    return ret

_check_for_existing = _check_for_existing_nt if os.name == "nt" else lambda x: False



#-------------------------------------------------------------------------------
class Gui(flow.cmd.Cmd):
    """ Opens the clientspec for the current directory in P4V """
    p4vargs  = flow.cmd.Arg([str], "Additional arguments to pass to P4V")
    def main(self):
        self.print_info("Fetching Perforce info")

        username = p4utils.login()

        cwd = os.getcwd()
        print(f"Finding client for '{cwd}'; ", end="")
        client = p4utils.get_client_from_dir(cwd, username)
        if not client:
            print()
            self.print_error("No client found")
            return False

        client, client_dir = client
        print(client)

        p4_port = p4utils.get_p4_set("P4PORT")
        if not p4_port:
            self.print_error("P4PORT must be set to start P4V")
            return False
        print("Using P4PORT", p4_port)

        self.print_info("Starting P4V")
        if _check_for_existing(client):
            print("P4V with client", client, "is already open")
            return

        print("Client root;", client_dir)

        args = (
            "-p", p4_port,
            "-u", username,
            "-c", client,
        )

        p4v_cwd = Path(os.getenv("TEMP", client_dir)).resolve()

        print("p4v", *args)
        subprocess.Popen(("p4v", *args, *self.args.p4vargs), cwd=p4v_cwd)
        print("Done!")
