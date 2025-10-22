# ToyDistributedHash
A C based distributed hashing naming service. This is an implementation of the consistent hashing flat naming system.
## Features
### On Bootstrap
- lookup key
- Insert key value
- delete key
### on Name Server
- enter
- exit
## Technologies Used
- Language: C
- Developed in: Emacs
- Environment: Unix server
- UI: CLI
## Setup
1. Clone this repository
```bash
git clone https://github.com/jameswarren123/HashingRing.git
```
2. Open in a compatible C environment where multiple connections are possible
3. compile with makefile compile
4. Use ./nameserver bnConfigFile.txt for the bootstrap node and ./nameserver nsConfigFile.txt for all other nodes referencing the examlpes for format.
