# unfs3-raft: Server Setup & Usage Guide

This is the server code. You were added as a collaborator, but it’s public anyway:  
**[https://github.com/FaridZandi/unfs3-raft](https://github.com/FaridZandi/unfs3-raft)**

---

## Clone the Repository

```bash
git clone --recursive git@github.com:FaridZandi/unfs3-raft.git
```

---

## Build Instructions

**Install Dependencies:**

```bash
apt-get update
apt-get install -y autoconf gdb
apt-get install -y gcc flex bison libtirpc-dev
```

**Bootstrap & Configure:**

```bash
./bootstrap
./configure
```

**Initialize Git Submodules:**

```bash
git submodule update --init
```

**Build the Raft Library:**

```bash
cd raft
make
cd ..
```

**(Optional) Install Python Client Library:**

```bash
cd scripts/client
python setup.py install
cd ../..
```

**Build the Project:**

```bash
make
make install
```

**Note:**  
If you run into issues with `configure` (possibly due to your Linux version), copy these files as a workaround:

```bash
cp /usr/share/automake-*/install-sh .
cp /usr/share/automake-*/config.guess .
cp /usr/share/automake-*/config.sub .
```

---

## Running the Server

### TERMINAL #1

Change into the `scripts` directory and use the provided scripts to start the NFS daemons:

```bash
./start-nfs.sh 5
```
This will launch **5 instances** (inst1, inst2, ...).

To stop all instances later:

```bash
sudo ./stop-nfs.sh
```

---

### TERMINAL #2

Run the client test script:

```bash
cd scripts/client/tests
python test-loop-noportmap.py
```

---

## Monitoring

- Go back to **TERMINAL #1**.
- Check the log in `inst1/unfsd.out` to see server operations:
  - Example:  
    ```bash
    cat inst1/unfsd.out
    ```
- Note: Logs may also be in other instance directories if a different one is selected.
