#!/usr/bin/env python3
"""Deploy the LightPanda web-search gateway on the VM (additive, no-touch existing).

- SSH to the VM, SFTP server.js / Dockerfile / docker-compose.yml into /root/lightpanda-search-gw/
- Generate a random GATEWAY_TOKEN, write the VM-local .env
- docker compose up -d --build (attaches to existing xiaozhi_default network)
- Test GET /search?q=test from inside the VM
- Print the token + external URL so it can go into jetson/.env
"""
import os
import sys
import time
import secrets
import getpass
import paramiko

VM_HOST = "36.50.27.142"
VM_USER = "root"
REMOTE_DIR = "/root/lightpanda-search-gw"
LOCAL_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)))

FILES = ["server.js", "Dockerfile", "docker-compose.yml"]


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
    # Never keep the VM credential in source control. CI/automation can inject
    # JETSON_VM_PASSWORD; interactive deploys use a hidden prompt.
    vm_password = os.environ.get("JETSON_VM_PASSWORD")
    if not vm_password:
        vm_password = getpass.getpass(f"Password for {VM_USER}@{VM_HOST}: ")
    ssh = paramiko.SSHClient()
    ssh.set_missing_host_key_policy(paramiko.AutoAddPolicy())
    ssh.connect(VM_HOST, username=VM_USER, password=vm_password, timeout=30)

    # Make sure the external network exists (it should; create if missing — harmless).
    run(ssh, "docker network ls --format '{{.Name}}' | grep -qx xiaozhi_default && "
             "echo 'network xiaozhi_default present' || "
             "(echo 'WARN: xiaozhi_default missing — creating' && docker network create xiaozhi_default)")

    # Confirm the existing lightpanda container + its CDP port is reachable from a container on that network.
    run(ssh, "docker ps --format '{{.Names}}\t{{.Status}}' | grep -i lightpanda || echo 'no lightpanda container found'")

    # SFTP the files up.
    print(f"\nSFTP -> {REMOTE_DIR}")
    run(ssh, f"mkdir -p {REMOTE_DIR}")
    # Reuse the token from the VM-local .env if present (idempotent redeploys);
    # otherwise generate a fresh one. NB: keep every TOKEN assignment on this
    # local — a bare module-level TOKEN read after an in-function assignment is
    # an UnboundLocalError on first-ever deploys (no .env on the VM yet).
    TOKEN = "lp_" + secrets.token_hex(20)
    try:
        sftp_tmp = ssh.open_sftp()
        with sftp_tmp.open(f"{REMOTE_DIR}/.env", "r") as fh:
            data = fh.read()
        sftp_tmp.close()
        for line in data.decode("utf-8", "replace").splitlines():
            line = line.strip()
            if line.startswith("GATEWAY_TOKEN=") and line.split("=", 1)[1]:
                TOKEN = line.split("=", 1)[1]
                print(f"  reusing existing GATEWAY_TOKEN=<{TOKEN[:7]}...>")
                break
    except IOError:
        pass
    sftp = ssh.open_sftp()
    for f in FILES:
        local = os.path.join(LOCAL_DIR, f)
        remote = f"{REMOTE_DIR}/{f}"
        sftp.put(local, remote)
        print(f"  put {f}")
    # VM-local .env with the token (not committed anywhere).
    with sftp.open(f"{REMOTE_DIR}/.env", "w") as fh:
        fh.write(f"GATEWAY_TOKEN={TOKEN}\n")
    sftp.close()
    print(f"  wrote .env (GATEWAY_TOKEN=<{TOKEN[:7]}...>)")

    # Build + run.
    run(ssh, f"cd {REMOTE_DIR} && docker compose up -d --build", timeout=600)

    # Give it a moment to start.
    time.sleep(3)
    run(ssh, f"docker ps --filter name=lightpanda-search-gw --format '{{.Names}}\t{{.Status}}\t{{.Ports}}'")
    run(ssh, "docker logs --tail 30 lightpanda-search-gw")

    # Internal tests from the VM host.
    print("\n=== internal test: /search (VM host -> localhost:9233) ===")
    run(ssh, f"curl -sS -m 60 -H 'Authorization: Bearer {TOKEN}' "
             "'http://localhost:9233/search?q=jetson%20nano' | head -c 2000")
    print("\n=== internal test: /fetch (lightpanda CDP + fallback) ===")
    run(ssh, f"curl -sS -m 90 -H 'Authorization: Bearer {TOKEN}' "
             "'http://localhost:9233/fetch?url=https%3A%2F%2Fexample.com' | head -c 800")

    ssh.close()

    print("\n" + "=" * 60)
    print("DEPLOY DONE")
    print("=" * 60)
    print(f"GATEWAY_TOKEN = {TOKEN}")
    print(f"LIGHTPANDA_SEARCH_URL  = http://{VM_HOST}:9233/search")
    print(f"LIGHTPANDA_SEARCH_TOKEN= {TOKEN}")
    print("Put these into jetson/.env (gitignored).")


if __name__ == "__main__":
    try:
        main()
    except Exception as e:
        print(f"DEPLOY FAILED: {e}", file=sys.stderr)
        sys.exit(1)
