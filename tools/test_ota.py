"""Hardware OTA checks; requires SMARTINFO_OTA_TOKEN and a device on the LAN.

python tools/test_ota.py DEVICE_IP --image build/SmartInfo7.bin --update
Add --timeout-test for a three-minute stalled upload check.
"""

import argparse
import hashlib
import http.client
import json
import os
from pathlib import Path
import struct
import subprocess
import time


class Device:
    def __init__(self, host, token):
        self.host = host
        self.token = token

    def request(self, method, path, body=None, headers=None, authorized=True):
        request_headers = {"Connection": "close"}
        if authorized:
            request_headers["Authorization"] = "Bearer " + self.token
        if method == "POST":
            request_headers["Content-Type"] = "application/octet-stream"
        request_headers.update(headers or {})
        timeout = 200 if method == "POST" and body and len(body) > 4096 else 10
        conn = http.client.HTTPConnection(self.host, timeout=timeout)
        try:
            conn.request(method, path, body=body, headers=request_headers)
            reply = conn.getresponse()
            return reply.status, reply.read()
        finally:
            conn.close()

    def status(self):
        code, body = self.request("GET", "/status")
        assert code == 200, (code, body)
        return json.loads(body)

    def settled(self, timeout=60):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            try:
                status = self.status()
                if not status["pending_verify"] and status["ota_phase"] not in (
                    "uploading", "verifying", "rebooting"
                ):
                    return status
            except (OSError, http.client.HTTPException):
                pass
            time.sleep(1)
        raise AssertionError("Device did not become ready")


def changed_image(image, *, project=None, chip=None):
    """Keep ESP image checksum/SHA valid while changing its identity for rejection tests."""
    result = bytearray(image)
    assert result[0] == 0xE9 and result[23] == 1, "Expected SHA-appended ESP-IDF image"
    assert struct.unpack_from("<I", result, 32)[0] == 0xABCD5432, "Missing app descriptor"
    if project is not None:
        result[80:112] = project.encode().ljust(32, b"\0")
    if chip is not None:
        struct.pack_into("<H", result, 12, chip)
    position = 24
    checksum = 0xEF
    for _ in range(result[1]):
        length = struct.unpack_from("<I", result, position + 4)[0]
        position += 8
        for value in result[position:position + length]:
            checksum ^= value
        position += length
    checksum_position = position + (15 - position % 16) % 16
    result[checksum_position] = checksum
    end = checksum_position + 1
    result[end:end + 32] = hashlib.sha256(result[:end]).digest()
    return bytes(result)


def check_rejection(device, expected, *, body=b"\0" * 1024, headers=None, authorized=True):
    before = device.settled()
    code, response = device.request("POST", "/ota", body, headers, authorized)
    assert code == expected, (code, response)
    after = device.settled()
    assert (before["slot"], before["build_hash"]) == (after["slot"], after["build_hash"])


def check_incomplete_upload(device, image, stalled):
    before = device.settled()
    conn = http.client.HTTPConnection(device.host, timeout=10)
    try:
        conn.putrequest("POST", "/ota")
        conn.putheader("Authorization", "Bearer " + device.token)
        conn.putheader("Content-Type", "application/octet-stream")
        conn.putheader("Content-Length", str(len(image)))
        conn.endheaders()
        conn.send(image[:4096])
        time.sleep(1)
        assert device.status()["ota_phase"] == "uploading", "Status blocked during upload"
        code, _ = device.request("POST", "/ota", b"\0" * 1024)
        assert code == 409, "Concurrent update was not rejected"
        if stalled:
            deadline = time.monotonic() + 190
            while time.monotonic() < deadline:
                status = device.status()
                if status["ota_phase"] == "failed":
                    assert status["ota_error"] == "ESP_ERR_TIMEOUT", status
                    break
                print("Waiting for stalled upload deadline...", flush=True)
                time.sleep(15)
            else:
                raise AssertionError("Stalled upload did not time out")
    finally:
        conn.close()
    after = device.settled()
    assert (before["slot"], before["build_hash"]) == (after["slot"], after["build_hash"])


def check_update(device, image):
    before = device.settled()
    code, body = device.request("POST", "/ota", image)
    assert code == 200, (code, body)
    time.sleep(3)
    after = device.settled(90)
    assert after["slot"] != before["slot"], (before, after)
    expected_hash = image[32 + 144:32 + 176].hex()
    assert after["build_hash"] == expected_hash, after
    print("PASS: OTA booted and confirmed", after["slot"], flush=True)


def check_rollback(device, image):
    before = device.settled()
    code, body = device.request("POST", "/ota", image)
    assert code == 200, (code, body)
    time.sleep(40)
    after = device.settled(90)
    assert (before["slot"], before["build_hash"]) == (after["slot"], after["build_hash"])
    print("PASS: failed boot rolled back to the previous confirmed firmware", flush=True)


def check_batch_helper(device, image_path):
    assert os.name == "nt", "The batch helper requires Windows"
    root = Path(__file__).resolve().parent.parent
    script = str(root / "ota_update.bat")
    for arguments in ([], ["http://invalid/"], [device.host, "missing-firmware.bin"]):
        result = subprocess.run(["cmd.exe", "/d", "/c", script, *arguments], cwd=root)
        assert result.returncode != 0, "Invalid batch arguments were accepted"
    bad_env = dict(os.environ, SMARTINFO_OTA_TOKEN="invalid")
    result = subprocess.run(["cmd.exe", "/d", "/c", script, device.host],
                            cwd=root, env=bad_env)
    assert result.returncode != 0, "Malformed token was accepted"
    before = device.settled()
    fallback_env = dict(os.environ)
    fallback_env.pop("SMARTINFO_OTA_TOKEN", None)
    result = subprocess.run(["cmd.exe", "/d", "/c", script, device.host, str(image_path.resolve())],
                            cwd=root, env=fallback_env)
    assert result.returncode == 0, "Batch helper failed to upload and confirm the firmware"
    after = device.status()
    assert after["slot"] != before["slot"] and not after["pending_verify"]
    assert after["build_hash"] == image_path.read_bytes()[176:208].hex()
    print("PASS: batch argument/token checks, upload, reboot, exact build and boot confirmation", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("host")
    parser.add_argument("--image", type=Path, default=Path("build/SmartInfo7.bin"))
    parser.add_argument("--update", action="store_true", help="Update both slots with --image")
    parser.add_argument("--timeout-test", action="store_true")
    parser.add_argument("--batch-helper", action="store_true", help="Test the Windows helper; writes firmware and reboots")
    parser.add_argument("--rollback-image", type=Path, help="Test-only image that fails boot health checks")
    args = parser.parse_args()
    token = os.environ.get("SMARTINFO_OTA_TOKEN", "")
    if len(token) != 64:
        parser.error("Set SMARTINFO_OTA_TOKEN to the configured bearer token")
    device = Device(args.host, token)
    image = args.image.read_bytes()
    assert changed_image(image) == image, "Test image checksum parser disagrees with built firmware"
    assert device.request("GET", "/status", authorized=False)[0] == 401
    assert device.request("GET", "/status", headers={"Authorization": "Bearer " + "z" * 64})[0] == 401
    check_rejection(device, 401, authorized=False)
    check_rejection(device, 415, headers={"Content-Type": "multipart/form-data"})
    check_rejection(device, 413, body=b"")
    check_rejection(device, 413, body=b"\xe9", headers={"Content-Length": str(0x1F0001)})
    check_rejection(device, 400)
    print("PASS: authentication, content type, size, invalid image", flush=True)
    check_rejection(device, 400, body=changed_image(image, project="OtherProject"))
    check_rejection(device, 400, body=changed_image(image, chip=9))
    print("PASS: valid-checksum images for another project/chip rejected", flush=True)
    check_incomplete_upload(device, image, False)
    check_incomplete_upload(device, image, False)
    print("PASS: upload worker accepts another request after repeated interruptions", flush=True)
    print("PASS: status remains responsive; concurrent and interrupted uploads handled", flush=True)
    if args.timeout_test:
        check_incomplete_upload(device, image, True)
        print("PASS: stalled upload times out without selecting a new slot", flush=True)
    if args.update:
        check_update(device, image)
        check_update(device, image)
    if args.rollback_image:
        check_rollback(device, args.rollback_image.read_bytes())
    if args.batch_helper:
        check_batch_helper(device, args.image)


if __name__ == "__main__":
    main()
