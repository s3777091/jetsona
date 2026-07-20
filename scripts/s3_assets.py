#!/usr/bin/env python3
"""Fetch (or seed) runtime assets for the jetsona firmware from MinIO (S3).

Pure standard library -- no mc / awscli / boto3 dependency, so it runs on the
Jetson (Ubuntu 18.04, python3.6) without extra installs.

Usage
-----
  python3 scripts/s3_assets.py fetch     # build-time: download missing assets
  uv run --script scripts/s3_assets.py upload
                                         # sync changed ./assets -> bucket
  uv run --script scripts/s3_assets.py upload-file icons/app/reload.png
                                         # upload one changed local asset
  uv run --script scripts/s3_assets.py delete-file icons/app/old.png
                                         # delete selected local/S3 asset keys
  python3 scripts/s3_assets.py list      # debug: list objects in the bucket
  python3 scripts/s3_assets.py fetch-file fonts/cloud/Example.ttf
                                         # runtime: fetch exactly one asset

`fetch` is the "bo kiem tra" (checker): it lists every object in the bucket and
downloads ONLY objects whose local file is missing or whose ETag/MD5 differs
from the remote. This detects a changed icon even when its byte size is
unchanged. Multipart objects without a simple MD5 ETag fall back to size.

Clock-skew safe: MinIO rejects SigV4 requests when the client clock is more
than ~15 minutes off. Instead of trusting the local clock we ask the server for
its time (Date header) once at start and sign every request with that. The
whole run is fast enough to stay inside the skew window.

Configuration (config.yaml and .env are loaded by the shell wrappers):
  MINIO_ENDPOINT    e.g. https://s3.phuongdong.cloud   (config.yaml, required)
  MINIO_ACCESS_KEY                                  (required)
  MINIO_SECRET_KEY                                  (required)
  MINIO_BUCKET      e.g. jetsona-assets              (config.yaml)
  MINIO_REGION      e.g. us-east-1                   (config.yaml)
  JETSON_ASSETS_DIR local assets dir                (default ./assets)
  ASSETS_S3_PREFIX   key prefix inside the bucket    (default "")
  ASSETS_VERBOSE    1 = print every skip/download    (default 1)
"""

import hashlib
import hmac
import http.client
import os
import sys
import urllib.parse
from collections import namedtuple
from datetime import datetime, timezone
from xml.etree import ElementTree

Response = namedtuple("Response", "status headers body")

# --------------------------------------------------------------------------- #
# Config
# --------------------------------------------------------------------------- #

DEFAULT_BUCKET = "jetsona-assets"
DEFAULT_REGION = "us-east-1"
DEFAULT_ASSETS_DIR = "assets"


def _unquote_flat_value(value):
    value = value.strip()
    if (len(value) >= 2 and value[0] == value[-1] and
            value[0] in ("'", '"')):
        return value[1:-1]
    return value


def _load_project_defaults():
    """Load the repository's flat config when no shell wrapper did it.

    Build/install wrappers already export these values. Direct CLI usage from
    README (including delete-file) also needs to work without echoing secrets.
    Existing process environment always wins.
    """
    repo_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    config_keys = {
        "MINIO_ENDPOINT", "MINIO_BUCKET", "MINIO_REGION",
        "JETSON_ASSETS_DIR", "ASSETS_S3_PREFIX", "ASSETS_VERBOSE",
    }
    config_path = os.path.join(repo_root, "config.yaml")
    if os.path.isfile(config_path):
        with open(config_path, "r", encoding="utf-8") as stream:
            for raw in stream:
                line = raw.strip()
                if not line or line.startswith("#") or ":" not in line:
                    continue
                key, value = line.split(":", 1)
                key = key.strip()
                if key in config_keys and key not in os.environ:
                    os.environ[key] = _unquote_flat_value(value)

    secret_keys = {"MINIO_ACCESS_KEY", "MINIO_SECRET_KEY"}
    env_path = os.path.join(repo_root, ".env")
    if os.path.isfile(env_path):
        with open(env_path, "r", encoding="utf-8") as stream:
            for raw in stream:
                line = raw.strip()
                if not line or line.startswith("#") or "=" not in line:
                    continue
                key, value = line.split("=", 1)
                key = key.strip()
                if key in secret_keys and key not in os.environ:
                    os.environ[key] = _unquote_flat_value(value)


def cfg():
    endpoint = os.environ.get("MINIO_ENDPOINT", "").rstrip("/")
    if not endpoint:
        die("MINIO_ENDPOINT is not set. Check config.yaml.")
    return {
        "endpoint": endpoint,
        "access_key": os.environ.get("MINIO_ACCESS_KEY", ""),
        "secret_key": os.environ.get("MINIO_SECRET_KEY", ""),
        "bucket": os.environ.get("MINIO_BUCKET", DEFAULT_BUCKET),
        "region": os.environ.get("MINIO_REGION", DEFAULT_REGION),
        "assets_dir": os.environ.get("JETSON_ASSETS_DIR", DEFAULT_ASSETS_DIR),
        "prefix": os.environ.get("ASSETS_S3_PREFIX", ""),
        "verbose": os.environ.get("ASSETS_VERBOSE", "1") == "1",
    }


def die(msg, code=1):
    sys.stderr.write("s3_assets: " + msg + "\n")
    sys.exit(code)


def log(msg):
    if os.environ.get("ASSETS_VERBOSE", "1") == "1":
        sys.stdout.write(msg + "\n")


# --------------------------------------------------------------------------- #
# Minimal HTTP / S3 (path-style, SigV4)
# --------------------------------------------------------------------------- #

class S3:
    def __init__(self, endpoint, access_key, secret_key, region, bucket):
        u = urllib.parse.urlparse(endpoint)
        self.scheme = u.scheme or "https"
        self.host = u.hostname
        self.port = u.port
        self.access_key = access_key
        self.secret_key = secret_key
        self.region = region
        self.bucket = bucket
        # Seed with the local clock only to sign the time-probe request; the
        # probe response replaces this with the server's clock for all later
        # requests. If the local clock is badly skewed the probe itself can
        # fail -- fall back to an anonymous (unsigned) HEAD just to read Date.
        self._server_time = datetime.now(timezone.utc)
        self._server_time = self._fetch_server_time()

    # -- low-level HTTPS helpers ------------------------------------------------
    def _conn(self):
        if self.scheme == "https":
            return http.client.HTTPSConnection(self.host, self.port or 443,
                                               timeout=60)
        return http.client.HTTPConnection(self.host, self.port or 80, timeout=60)

    def _fetch_server_time(self):
        """Read the server's clock via an anonymous HEAD (no signature needed).

        We only need the Date response header, which the server returns even on
        a 403. Using the server's own time to sign every later request makes the
        client immune to local clock skew (MinIO rejects SigV4 if >~15 min off).
        """
        conn = self._conn()
        try:
            conn.request("HEAD", "/" + _path_quote(self.bucket))
            resp = conn.getresponse()
            resp.read()
            date_hdr = resp.getheader("Date")
        finally:
            conn.close()
        # If HEAD is not allowed the server still returns the Date header on a
        # 403/405; fall back to an anonymous GET of the service root for it.
        if not date_hdr:
            conn = self._conn()
            try:
                conn.request("GET", "/")
                resp = conn.getresponse()
                resp.read()
                date_hdr = resp.getheader("Date")
            finally:
                conn.close()
        if not date_hdr:
            die("server returned no Date header; cannot sign safely.")
        # RFC 7231 format: "Thu, 16 Jul 2026 03:02:53 GMT"
        t = datetime.strptime(date_hdr, "%a, %d %b %Y %H:%M:%S %Z")
        return t.replace(tzinfo=timezone.utc)

    # ------------------------------------------------------------------
    def _sign(self, method, path, query, headers, payload_sha):
        t = self._server_time
        amz_date = t.strftime("%Y%m%dT%H%M%SZ")
        datestamp = t.strftime("%Y%m%d")

        canon_headers = "".join(
            "{}:{}\n".format(k.lower(), " ".join(str(headers[k]).strip().split()))
            for k in sorted(headers.keys()))
        signed_headers = ";".join(k.lower() for k in sorted(headers.keys()))

        canon_query = "&".join(
            "{}={}".format(_uri_quote(k), _uri_quote(v))
            for k, v in sorted(query.items()))

        canon_req = "\n".join([
            method,
            path,                       # already URI-encoded path
            canon_query,
            canon_headers,
            signed_headers,
            payload_sha,
        ])

        scope = "{}/{}/s3/aws4_request".format(datestamp, self.region)
        sts = "AWS4-HMAC-SHA256\n{}\n{}\n{}".format(
            amz_date, scope, _sha256_hex(canon_req))

        k_date = _hmac(("AWS4" + self.secret_key).encode(), datestamp)
        k_region = _hmac(k_date, self.region)
        k_service = _hmac(k_region, "s3")
        k_signing = _hmac(k_service, "aws4_request")
        sig = _hmac(k_signing, sts.encode("utf-8")).hex()

        headers = dict(headers)
        headers["x-amz-date"] = amz_date
        headers["x-amz-content-sha256"] = payload_sha
        headers["Authorization"] = (
            "AWS4-HMAC-SHA256 Credential={}/{}, SignedHeaders={}, Signature={}"
            .format(self.access_key, scope, signed_headers, sig))
        return headers

    def _request(self, method, path, query=None, headers=None, body=b""):
        query = query or {}
        # Headers we always sign: host (must be present) + the two amz headers
        # are added by _sign. Anything extra the caller passes is merged.
        base_headers = {"host": self.host}
        if headers:
            base_headers.update(headers)
        payload_sha = _sha256_hex(body or b"")
        signed = self._sign(method, path, query, base_headers, payload_sha)

        url = path
        if query:
            url += "?" + "&".join(
                "{}={}".format(_uri_quote(k), _uri_quote(v))
                for k, v in query.items())

        conn = self._conn()
        try:
            conn.request(method, url, body=body, headers=signed)
            resp = conn.getresponse()
            data = resp.read()
        finally:
            conn.close()
        return Response(resp.status, {k.lower(): v for k, v in resp.getheaders()}, data)

    # We need a bucket reference for _fetch_server_time before __init__ sets
    # attrs; use a module-level default via the config instead.
    _default_bucket = None


def _hmac(key, msg):
    if isinstance(msg, str):
        msg = msg.encode("utf-8")
    return hmac.new(key, msg, hashlib.sha256).digest()


def _sha256_hex(data):
    if isinstance(data, str):
        data = data.encode("utf-8")
    return hashlib.sha256(data).hexdigest()


def _uri_quote(s):
    # SigV4 query keys/values use RFC 3986 unreserved bytes only.  In
    # particular '/' inside a prefix must be %2F; leaving it literal makes
    # MinIO calculate a different canonical request (fetch-file -> 403).
    # Object paths are encoded separately by _path_quote(), which preserves
    # their segment separators.
    return urllib.parse.quote(str(s), safe="-_.~")


def _path_quote(s):
    # Path-style: encode each segment but keep the slashes.
    return "/".join(urllib.parse.quote(p, safe="") for p in s.split("/"))


# --------------------------------------------------------------------------- #
# Object operations
# --------------------------------------------------------------------------- #

def list_objects(s3, bucket, prefix=""):
    """Return (key, size, etag) for every object under prefix."""
    objects = []
    continuation = None
    while True:
        query = {"list-type": "2"}
        if prefix:
            query["prefix"] = prefix
        if continuation:
            query["continuation-token"] = continuation
        resp = s3._request("GET", path="/" + _path_quote(bucket),
                           query=query)
        body = resp.body
        if os.environ.get("ASSETS_DEBUG"):
            sys.stderr.write("DEBUG list: HTTP {} body={!r}\n".format(resp.status, body[:400]))
        if resp.status != 200:
            die("ListObjectsV2 failed: HTTP {} {}".format(resp.status, _body_text(body)))
        ns = {"s3": "http://s3.amazonaws.com/doc/2006-03-01/"}
        root = ElementTree.fromstring(body)
        for c in root.findall("s3:Contents", ns):
            key = c.findtext("s3:Key", default="", namespaces=ns)
            size = int(c.findtext("s3:Size", default="0", namespaces=ns))
            etag = c.findtext("s3:ETag", default="", namespaces=ns).strip('"')
            objects.append((key, size, etag))
        ct = root.findtext("s3:NextContinuationToken", default=None, namespaces=ns)
        is_trunc = root.findtext("s3:IsTruncated", default="false", namespaces=ns)
        if is_trunc != "true" or not ct:
            break
        continuation = ct
    return objects


def file_md5(path):
    digest = hashlib.md5()
    with open(path, "rb") as stream:
        while True:
            chunk = stream.read(1024 * 1024)
            if not chunk:
                break
            digest.update(chunk)
    return digest.hexdigest()


def local_object_matches(path, expected_size, etag):
    if not os.path.isfile(path) or os.path.getsize(path) != expected_size:
        return False
    # A normal single-part S3/MinIO ETag is the object's MD5. Multipart ETags
    # contain a dash and cannot be reproduced without knowing part boundaries.
    normalized = etag.lower()
    if (len(normalized) == 32 and
            all(ch in "0123456789abcdef" for ch in normalized)):
        return file_md5(path) == normalized
    return True


def download_object(s3, bucket, key, dest_path, expected_size):
    resp = s3._request("GET", path="/" + _path_quote(bucket + "/" + key))
    data = resp.body
    if resp.status != 200:
        die("download {} failed: HTTP {} {}".format(key, resp.status, _body_text(data)))
    if len(data) != expected_size:
        die("size mismatch for {}: got {} expected {}".format(key, len(data), expected_size))
    os.makedirs(os.path.dirname(dest_path) or ".", exist_ok=True)
    tmp = dest_path + ".part"
    with open(tmp, "wb") as f:
        f.write(data)
    os.replace(tmp, dest_path)


def put_object(s3, bucket, key, src_path):
    with open(src_path, "rb") as f:
        body = f.read()
    # Content type is cosmetic for PNG/TTF; default octet-stream is fine.
    resp = s3._request("PUT", path="/" + _path_quote(bucket + "/" + key),
                       headers={"content-length": str(len(body))}, body=body)
    if resp.status not in (200, 201, 204):
        die("upload {} failed: HTTP {} {}".format(key, resp.status, _body_text(resp.body)))


def delete_object(s3, bucket, key):
    resp = s3._request("DELETE", path="/" + _path_quote(bucket + "/" + key))
    if resp.status not in (200, 202, 204):
        die("delete {} failed: HTTP {} {}".format(key, resp.status, _body_text(resp.body)))


def ensure_bucket(s3, bucket, region):
    """Create the bucket if it doesn't exist. Idempotent."""
    resp = s3._request("HEAD", path="/" + _path_quote(bucket))
    if resp.status == 200:
        return False  # already exists
    # us-east-1 needs an empty body; other regions need a LocationConstraint.
    body = b""
    headers = {"content-length": "0"}
    if region != "us-east-1":
        body = ('<CreateBucketConfiguration xmlns="http://s3.amazonaws.com/doc/2006-03-01/">'
                '<LocationConstraint>{}</LocationConstraint></CreateBucketConfiguration>'
                .format(region)).encode()
        headers["content-length"] = str(len(body))
    resp = s3._request("PUT", path="/" + _path_quote(bucket),
                       headers=headers, body=body)
    if resp.status not in (200, 201, 204):
        die("create bucket {} failed: HTTP {} {}".format(bucket, resp.status, _body_text(resp.body)))
    return True


def _body_text(b):
    try:
        return b.decode("utf-8", "replace")[:300]
    except Exception:
        return "<binary>"


# --------------------------------------------------------------------------- #
# Commands
# --------------------------------------------------------------------------- #

def _make_s3(c):
    if not c["access_key"] or not c["secret_key"]:
        die("MINIO_ACCESS_KEY / MINIO_SECRET_KEY not set. Check .env.")
    return S3(c["endpoint"], c["access_key"], c["secret_key"], c["region"], c["bucket"])


def cmd_list(c):
    s3 = _make_s3(c)
    objs = list_objects(s3, c["bucket"], c["prefix"])
    print("bucket '{}' : {} objects".format(c["bucket"], len(objs)))
    for key, size, _etag in objs:
        print("  {:>10}  {}".format(size, key))


def _trashed_asset_paths(assets_dir):
    """Paths intentionally absent because the runtime moved them to Trash.

    Without this guard a later build-time fetch would interpret a trashed
    wallpaper as a missing cache entry and silently download it again.
    """
    result = set()
    trash_dir = os.path.join(assets_dir, "backgrounds", ".trash")
    if not os.path.isdir(trash_dir):
        return result
    def add_original(original_name):
        if not original_name or "/" in original_name or "\\" in original_name:
            return
        result.add("backgrounds/" + original_name)
        result.add("backgrounds/thumbs/" + original_name)

    for stored_name in os.listdir(trash_dir):
        if stored_name.endswith(".thumb") or "__" not in stored_name:
            continue
        add_original(stored_name.split("__", 1)[1])

    # Empty Trash deletes the image bytes but leaves a zero-byte marker so a
    # later MinIO sync cannot resurrect a permanently deleted wallpaper.
    deleted_dir = os.path.join(trash_dir, ".deleted")
    if os.path.isdir(deleted_dir):
        for original_name in os.listdir(deleted_dir):
            add_original(original_name)
    return result


def cmd_fetch(c):
    s3 = _make_s3(c)
    assets_dir = c["assets_dir"]
    prefix = c["prefix"]
    objs = list_objects(s3, c["bucket"], prefix)
    print("==> fetch: {} objects in '{}', target dir '{}'".format(
        len(objs), c["bucket"], assets_dir))
    trashed_paths = _trashed_asset_paths(assets_dir)
    downloaded = skipped = 0
    for key, size, etag in objs:
        rel = key[len(prefix):] if prefix and key.startswith(prefix) else key
        rel = rel.lstrip("/")
        if not rel or rel.endswith("/"):
            continue  # synthetic dir entry
        # Cloud font binaries are intentionally on-demand. The small catalog
        # remains part of the normal asset sync, but opening Settings > General
        # > Fonts is what downloads an actual TTF. Set the override only when
        # preparing a fully offline image that should contain every font.
        if (rel.replace("\\", "/").startswith("fonts/cloud/") and
                rel.lower().endswith(".ttf") and
                os.environ.get("ASSETS_FETCH_CLOUD_FONTS", "0") != "1"):
            skipped += 1
            log("  cloud   {} (on demand)".format(rel))
            continue
        if rel.replace("\\", "/") in trashed_paths:
            skipped += 1
            log("  trash   {}".format(rel))
            continue
        dest = os.path.join(assets_dir, rel.replace("/", os.sep))
        if local_object_matches(dest, size, etag):
            skipped += 1
            log("  skip    {}".format(rel))
            continue
        download_object(s3, c["bucket"], key, dest, size)
        downloaded += 1
        log("  fetch   {}".format(rel))
    print("==> done: {} downloaded, {} skipped (present or in Trash)".format(
        downloaded, skipped))
    if downloaded == 0 and skipped == 0:
        die("bucket is empty or prefix '{}' matched nothing.".format(prefix or "/"), code=2)


def cmd_fetch_file(c, rel):
    """Download one caller-selected asset below JETSON_ASSETS_DIR.

    The UI passes only keys from its S3-owned font manifest. Reject absolute
    paths and traversal here as a second boundary before constructing either
    the remote key or local destination.
    """
    rel = rel.replace("\\", "/")
    if (not rel or rel.startswith("/") or rel.endswith("/") or
            any(part in ("", ".", "..") for part in rel.split("/"))):
        die("fetch-file requires a safe relative object key.", code=2)
    s3 = _make_s3(c)
    key = (c["prefix"] + rel) if c["prefix"] else rel
    matches = [(obj_key, size, etag)
               for obj_key, size, etag in list_objects(s3, c["bucket"], key)
               if obj_key == key]
    if not matches:
        die("object '{}' was not found in bucket '{}'.".format(key, c["bucket"]), code=2)
    _, size, etag = matches[0]
    dest = os.path.join(c["assets_dir"], *rel.split("/"))
    # Catalog refreshes are tiny and should see same-size content changes;
    # immutable font binaries can use the normal size-based cache.
    force = rel.lower().endswith("catalog.tsv")
    if not force and local_object_matches(dest, size, etag):
        print("==> fetch-file: cached {}".format(rel))
        return
    download_object(s3, c["bucket"], key, dest, size)
    print("==> fetch-file: downloaded {} ({} bytes)".format(rel, size))


def cmd_upload_files(c, paths):
    """Upload selected local assets without rewriting the rest of the bucket."""
    prepared = []
    for rel in paths:
        rel = rel.replace("\\", "/")
        if (not rel or rel.startswith("/") or rel.endswith("/") or
                any(part in ("", ".", "..") for part in rel.split("/"))):
            die("upload-file requires safe relative asset paths.", code=2)
        source = os.path.join(c["assets_dir"], *rel.split("/"))
        if not os.path.isfile(source):
            die("local asset '{}' was not found.".format(source), code=2)
        prepared.append((rel, source))
    s3 = _make_s3(c)
    ensure_bucket(s3, c["bucket"], c["region"])
    for rel, source in prepared:
        key = (c["prefix"] + rel) if c["prefix"] else rel
        put_object(s3, c["bucket"], key, source)
        print("==> upload-file: uploaded {} ({} bytes)".format(
            rel, os.path.getsize(source)))


def cmd_delete_files(c, paths):
    """Delete selected object keys without touching any sibling assets."""
    rels = []
    for rel in paths:
        rel = rel.replace("\\", "/")
        if (not rel or rel.startswith("/") or rel.endswith("/") or
                any(part in ("", ".", "..") for part in rel.split("/"))):
            die("delete-file requires safe relative asset paths.", code=2)
        rels.append(rel)

    s3 = _make_s3(c)
    for rel in rels:
        key = (c["prefix"] + rel) if c["prefix"] else rel
        matches = [obj_key for obj_key, _, _ in list_objects(s3, c["bucket"], key)
                   if obj_key == key]
        if not matches:
            print("==> delete-file: already absent {}".format(key))
            continue
        delete_object(s3, c["bucket"], key)
        remaining = [obj_key for obj_key, _, _ in list_objects(s3, c["bucket"], key)
                     if obj_key == key]
        if remaining:
            die("object '{}' still exists after DELETE.".format(key))
        print("==> delete-file: deleted {}".format(key))


def _iter_local_files(assets_dir):
    for root, dirs, files in os.walk(assets_dir):
        # Trash is local mutable state, not a source asset to seed back into
        # the shared bucket.
        dirs[:] = [name for name in dirs if name != ".trash"]
        for fn in files:
            full = os.path.join(root, fn)
            rel = os.path.relpath(full, assets_dir).replace(os.sep, "/")
            yield rel, full


def cmd_upload(c):
    s3 = _make_s3(c)
    assets_dir = c["assets_dir"]
    if not os.path.isdir(assets_dir):
        die("assets dir '{}' not found.".format(assets_dir))
    created = ensure_bucket(s3, c["bucket"], c["region"])
    print("==> bucket '{}' {}{}".format(
        c["bucket"], "created" if created else "exists",
        (" region=" + c["region"]) if created else ""))
    prefix = c["prefix"]
    files = list(_iter_local_files(assets_dir))
    remote = {key: (size, etag)
              for key, size, etag in list_objects(s3, c["bucket"], prefix)}
    print("==> upload: {} files from '{}' -> '{}/{}'".format(
        len(files), assets_dir, c["bucket"], prefix))
    uploaded = skipped = 0
    for rel, full in files:
        key = prefix + rel if prefix else rel
        remote_info = remote.get(key)
        if (remote_info and local_object_matches(full, remote_info[0], remote_info[1])):
            skipped += 1
            log("  skip    {}".format(rel))
            continue
        put_object(s3, c["bucket"], key, full)
        uploaded += 1
        log("  put     {}".format(rel))
    print("==> done: {} uploaded, {} unchanged.".format(uploaded, skipped))


COMMANDS = {"fetch": cmd_fetch, "upload": cmd_upload, "list": cmd_list}


def main(argv):
    _load_project_defaults()
    if len(argv) >= 2 and argv[1] == "fetch-file":
        if len(argv) != 3:
            die("usage: s3_assets.py fetch-file <relative-object-key>", code=2)
        cmd_fetch_file(cfg(), argv[2])
        return
    if len(argv) >= 2 and argv[1] == "upload-file":
        if len(argv) < 3:
            die("usage: s3_assets.py upload-file <relative-asset-path> [...]", code=2)
        cmd_upload_files(cfg(), argv[2:])
        return
    if len(argv) >= 2 and argv[1] == "delete-file":
        if len(argv) < 3:
            die("usage: s3_assets.py delete-file <relative-asset-path> [...]", code=2)
        cmd_delete_files(cfg(), argv[2:])
        return
    if len(argv) < 2 or argv[1] not in COMMANDS:
        die("usage: s3_assets.py <fetch|fetch-file|upload|upload-file|delete-file|list>\n"
            "  fetch  - download missing/mismatched assets from MinIO\n"
            "  fetch-file <key> - download exactly one on-demand asset\n"
            "  upload-file <path> - upload exactly one local asset\n"
            "  delete-file <path> - delete selected objects from MinIO\n"
            "  upload - seed ./assets into the MinIO bucket\n"
            "  list   - list objects in the bucket", code=2)
    COMMANDS[argv[1]](cfg())


if __name__ == "__main__":
    main(sys.argv)
