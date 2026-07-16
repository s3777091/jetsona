#!/usr/bin/env python3
"""Deploy the self-hosted "Studio" (code-server) on the VM (additive, no-touch).

- SSH to the VM, SFTP docker-compose.yml into /root/jetsona-code-server/
- Generate (or reuse) STUDIO_PASSWORD, write the VM-local .env
- Pre-create ./project and ./config owned by uid 1000 (container `coder` user)
- docker compose up -d, then test GET /healthz from inside the VM
- Print JETSON_STUDIO_URL (goes into jetson/.env) + the login password

The Jetson Studio tile opens this URL in the Chromium kiosk whenever no
RunPod GPU pod is running. Same credential rule as the search gateway:
nothing is committed; interactive deploys prompt, automation can inject
JETSON_VM_PASSWORD.
"""
import os
import sys
import time
import secrets
import getpass
import paramiko

VM_HOST = "36.50.27.142"
VM_USER = "root"
REMOTE_DIR = "/root/jetsona-code-server"
LOCAL_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)))

FILES = ["docker-compose.yml"]


def run(ssh, cmd, timeout=300):
    print(f"\n$ {cmd}")
    stdin, stdout, stderr = ssh.exec_command(cmd, timeout=timeout, get_pty=False)
    out = stdout.read().decode("utf-8", "replace")
    err = stderr.read().decode("utf-8", "replace")
    rc = stdout.channel.recv_exit_status()
    if out.strip():
        print(out.rstrip())
    if err.strip():
        print("[stderr]", err.rstrip())
    print(f"[exit {rc}]")
    return rc, out, err


def main():
    print(f"Connecting to {VM_USER}@{VM_HOST} ...")
    vm_password = os.environ.get("JETSON_VM_PASSWORD")
    if not vm_password:
        vm_password = getpass.getpass(f"Password for {VM_USER}@{VM_HOST}: ")
    ssh = paramiko.SSHClient()
    ssh.set_missing_host_key_policy(paramiko.AutoAddPolicy())
    ssh.connect(VM_HOST, username=VM_USER, password=vm_password, timeout=30)

    print(f"\nSFTP -> {REMOTE_DIR}")
    # Workspace + config dirs must exist before compose mounts them, and must
    # be writable by the container's non-root `coder` user (uid/gid 1000).
    run(ssh, f"mkdir -p {REMOTE_DIR}/project {REMOTE_DIR}/config && "
             f"chown -R 1000:1000 {REMOTE_DIR}/project {REMOTE_DIR}/config")

    # Reuse the password from the VM-local .env if present (idempotent
    # redeploys keep the login stable); otherwise generate a fresh one.
    password = "cs_" + secrets.token_hex(16)
    try:
        sftp_tmp = ssh.open_sftp()
        with sftp_tmp.open(f"{REMOTE_DIR}/.env", "r") as fh:
            data = fh.read()
        sftp_tmp.close()
        for line in data.decode("utf-8", "replace").splitlines():
            line = line.strip()
            if line.startswith("STUDIO_PASSWORD=") and line.split("=", 1)[1]:
                password = line.split("=", 1)[1]
                print(f"  reusing existing STUDIO_PASSWORD=<{password[:6]}...>")
                break
    except IOError:
        pass

    sftp = ssh.open_sftp()
    for f in FILES:
        sftp.put(os.path.join(LOCAL_DIR, f), f"{REMOTE_DIR}/{f}")
        print(f"  put {f}")
    with sftp.open(f"{REMOTE_DIR}/.env", "w") as fh:
        fh.write(f"STUDIO_PASSWORD={password}\n")
    sftp.close()
    print(f"  wrote .env (STUDIO_PASSWORD=<{password[:6]}...>)")

    run(ssh, f"cd {REMOTE_DIR} && docker compose up -d", timeout=600)

    time.sleep(4)
    run(ssh, "docker ps --filter name=jetsona-code-server "
             "--format '{{.Names}}\t{{.Status}}\t{{.Ports}}'")
    run(ssh, "docker logs --tail 20 jetsona-code-server")

    print("\n=== internal test (VM host -> localhost:8443) ===")
    run(ssh, "curl -sS -m 30 http://localhost:8443/healthz")

    ssh.close()

    print("\n" + "=" * 60)
    print("DEPLOY DONE")
    print("=" * 60)
    print(f"JETSON_STUDIO_URL=http://{VM_HOST}:8443")
    print(f"Mat khau dang nhap Studio: {password}")
    print("Dan JETSON_STUDIO_URL vao jetson/.env; mo cong 8443 tren firewall VM.")
    print("Neu VM da vao tailnet, dung URL IP Tailscale 100.x thay cho IP public")
    print("de khong phai mo cong ra internet (code-server chi co HTTP + password).")


if __name__ == "__main__":
    try:
        main()
    except Exception as e:
        print(f"DEPLOY FAILED: {e}", file=sys.stderr)
        sys.exit(1)
