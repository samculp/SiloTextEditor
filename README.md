# Silo Text Editor

A minimal terminal-based text editor written in C.

This project follows the [Build Your Own Text Editor](https://viewsourcecode.org/snaptoken/kilo/) tutorial by Snaptoken where you implement a text editor from scratch. I built this because I wanted to explore C, terminal I/O, and low-level system programming.

## Requirements

- GCC
- Linux or WSL
- Make (optional)

## Setup

Clone the repository:

```bash
git clone https://github.com/samculp/SiloTextEditor.git
cd SiloTextEditor
```

Compile:

```bash
gcc silo.c -o silo
```

Run:

```bash
./silo
```

To open an existing file:

```bash
./silo filename.txt
```

## Reference

[Build Your Own Text Editor – Snaptoken](https://viewsourcecode.org/snaptoken/kilo/)