# Unidocs Text Editor

<p align="center">
  A dual-mode text editor for the terminal and desktop.
</p>

<p align="center">
  <img src="https://img.shields.io/badge/Language-C%20%7C%20Python-2ea44f?style=flat-square" alt="C and Python">
  <img src="https://img.shields.io/badge/Interface-CLI%20%2B%20GUI-0969da?style=flat-square" alt="CLI and GUI">
  <img src="https://img.shields.io/badge/GUI-Tkinter-7c3aed?style=flat-square" alt="Tkinter GUI">
</p>

Unidocs Text Editor is a System Programming Lab project that offers two ways to work with text files: a responsive **C terminal editor** and a modern **Python/Tkinter graphical editor**. The `unified_editor.py` launcher lets you choose the experience that fits your workflow.

## Highlights

| Terminal editor | Graphical editor |
| --- | --- |
| Full-screen raw-terminal experience | Multi-tab desktop workspace |
| Built in C for a lightweight native workflow | Built with Python and Tkinter |
| Find, replace, selection, and undo/redo | Syntax highlighting and line numbers |
| Open, save, save as, file statistics, and run support | Themes, font controls, terminal panel, and run support |

## Screenshots

### Choose an editor

Run the unified launcher to select the terminal or graphical interface.

<p align="center">
  <img src="Assets/interface.png" alt="Unidocs launcher menu in a terminal" width="850">
</p>

### Graphical editor

The GUI provides tabs, language-aware syntax highlighting, line numbers, an integrated terminal panel, and one-click code execution.

<p align="center">
  <img src="Assets/gui.png" alt="Unidocs graphical editor with code and integrated terminal" width="900">
</p>

### Terminal editor

The CLI editor provides a focused full-screen editing environment with line numbers, a status bar, and keyboard-driven controls.

<p align="center">
  <img src="Assets/cli.png" alt="Unidocs terminal text editor editing sample.txt" width="900">
</p>

## Features

### CLI editor

- Open existing files or create new files
- Save and Save As
- Line numbers, cursor navigation, and terminal-resize support
- Select all, selection mode, cut, copy, and paste
- Undo and redo
- Find, replace, and go to line
- File statistics
- Run supported source files from the editor

### GUI editor

- Work with multiple files in tabs
- Open, save, Save As, and close-tab actions
- Find and replace
- Undo, redo, cut, copy, and paste
- Line numbers and live cursor-position status bar
- Dark and light themes
- Font selection and font-size controls
- Syntax highlighting for Python, C/C++, Java, JavaScript, and HTML
- Integrated terminal panel and run-current-file support

## Project structure

```text
Unidocs Text Editor/
├── Assets/
│   ├── cli.png           # Terminal editor screenshot
│   ├── gui.png           # Graphical editor screenshot
│   └── interface.png     # Launcher screenshot
├── unified_editor.py     # Main launcher for CLI and GUI modes
├── gui_editor.py         # Tkinter GUI editor implementation
├── cli_editor.c          # C terminal editor implementation
├── Makefile              # Build rules for the CLI editor
├── sample.txt            # Example text file
└── README.md             # Project documentation
```

## Requirements

- Linux, macOS, or another Unix-like system for the terminal editor
- A C compiler, such as `gcc` or `clang`
- `make`
- Python 3
- Tkinter for the GUI (`python3-tk` package on Debian/Ubuntu)

On Debian or Ubuntu, install the requirements with:

```bash
sudo apt update
sudo apt install build-essential make python3 python3-tk
```

## Getting started

Clone the project or open its directory, then run:

```bash
python3 unified_editor.py
```

Select one of the displayed options:

1. Command Line Interface (CLI)
2. Graphical User Interface (GUI)
3. Exit

You can also launch a mode directly:

```bash
# Start the graphical editor
python3 unified_editor.py --gui

# Start the terminal editor
python3 unified_editor.py --cli

# Open a file in the terminal editor
python3 unified_editor.py --cli sample.txt
```

## Build the terminal editor directly

```bash
make
./cli_editor sample.txt
```

Remove the compiled binary when needed:

```bash
make clean
```

## Keyboard shortcuts

### CLI editor

Press `Ctrl+T` inside the editor for the built-in help screen.

| Shortcut | Action |
| --- | --- |
| `Ctrl+S` / `Ctrl+W` | Save / Save As |
| `Ctrl+N` / `Ctrl+O` | New file / Open file |
| `Ctrl+Q` | Quit |
| `Ctrl+F` / `Ctrl+R` / `Ctrl+G` | Find / Replace / Go to line |
| `Ctrl+Z` / `Ctrl+Y` | Undo / Redo |
| `Ctrl+C` / `Ctrl+X` / `Ctrl+V` | Copy / Cut / Paste |
| `Ctrl+B` / `Ctrl+A` | Toggle selection / Select all |
| `Ctrl+E` / `Ctrl+P` | File statistics / Run current file |

### GUI editor

| Shortcut | Action |
| --- | --- |
| `Ctrl+N` / `Ctrl+O` / `Ctrl+S` | New tab / Open file / Save |
| `Ctrl+W` | Close tab |
| `Ctrl+F` | Find and replace |
| `Ctrl+R` or `F5` | Run current file |
| `Ctrl+Z` | Undo |
| `Ctrl+X` / `Ctrl+C` / `Ctrl+V` | Cut / Copy / Paste |

## License

This project is intended for academic and learning use.
