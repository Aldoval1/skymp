#!/bin/bash
# setup.sh - Installs Docker, Docker Compose, Git, and UFW on Ubuntu 24.04

set -e

echo "Updating system packages..."
sudo apt-get update -y
sudo apt-get upgrade -y

echo "Installing Git, UFW, and dependencies..."
sudo apt-get install -y git ufw ca-certificates curl

echo "Installing Docker..."
# Add Docker's official GPG key:
sudo install -m 0755 -d /etc/apt/keyrings
sudo curl -fsSL https://download.docker.com/linux/ubuntu/gpg -o /etc/apt/keyrings/docker.asc
sudo chmod a+r /etc/apt/keyrings/docker.asc

# Add the repository to Apt sources:
echo \
  "deb [arch=$(dpkg --print-architecture) signed-by=/etc/apt/keyrings/docker.asc] https://download.docker.com/linux/ubuntu \
  $(. /etc/os-release && echo "$VERSION_CODENAME") stable" | \
  sudo tee /etc/apt/sources.list.d/docker.list > /dev/null
sudo apt-get update -y

# Install Docker components
sudo apt-get install -y docker-ce docker-ce-cli containerd.io docker-buildx-plugin docker-compose-plugin

echo "Enabling and starting Docker service..."
sudo systemctl enable docker
sudo systemctl start docker

# Add current user to docker group to run without sudo (optional but recommended)
sudo usermod -aG docker $USER

echo "Setup is complete!"
