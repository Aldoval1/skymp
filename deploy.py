import os
import tarfile
import paramiko
import time

host = "104.238.137.223"
user = "root"
pwd = "9!FpK}MDSUe2S39k"
tar_name = "server_deploy.tar.gz"

def exclude_filter(tarinfo):
    excl = ['.git', 'node_modules', 'dist', 'build', '.xmake', '.vscode']
    for e in excl:
        if e in tarinfo.name.split('/') or tarinfo.name.endswith(e):
            return None
    return tarinfo

print("Compressing repository (skipping massive folders)...")
with tarfile.open(tar_name, "w:gz") as tar:
    for item in os.listdir(os.getcwd()):
        if item == tar_name:
            continue
        tar.add(item, arcname=item, filter=exclude_filter)
print("Compression done.")

print("Connecting to SSH...")
ssh = paramiko.SSHClient()
ssh.set_missing_host_key_policy(paramiko.AutoAddPolicy())
ssh.connect(host, username=user, password=pwd)

print("Connected! Uploading tarball...")
sftp = ssh.open_sftp()
sftp.put(tar_name, "/root/" + tar_name)
sftp.close()
print("Upload complete!")

commands = [
    "mkdir -p /root/st-roleplay",
    "tar -xzf /root/server_deploy.tar.gz -C /root/st-roleplay",
    "cd /root/st-roleplay && chmod +x setup.sh && ./setup.sh",
    "cd /root/st-roleplay && docker compose up -d --build"
]

for cmd in commands:
    print(f"Executing: {cmd}")
    stdin, stdout, stderr = ssh.exec_command(cmd)
    exit_status = stdout.channel.recv_exit_status()
    print("STDOUT:", stdout.read().decode())
    print("STDERR:", stderr.read().decode())

ssh.close()
print("Deployment executed successfully!")
