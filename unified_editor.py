#!/usr/bin/env python3
import sys
import os
import subprocess
import argparse

def launch_gui():
    try:
        import tkinter as tk
        import gui_editor
        print("Launching Graphical User Interface (GUI)...")
        root = tk.Tk()
        app = gui_editor.ModernNotepad(root)
        root.lift()
        root.attributes('-topmost', True)
        root.after_idle(root.attributes, '-topmost', False)
        root.mainloop()
    except ImportError as e:
        print(f"Error launching GUI: {e}")
        print("Make sure tkinter is installed on your system.")
        sys.exit(1)

def launch_cli(file_to_open=None):
    cli_executable = "./cli_editor"
    if not os.path.exists(cli_executable):
        print("CLI executable not found. Attempting to compile...")
        compile_process = subprocess.run(["make"], capture_output=True, text=True)
        if compile_process.returncode != 0:
            print("Failed to compile the CLI editor:")
            print(compile_process.stderr)
            sys.exit(1)
        print("Compilation successful.")
    
    print("Launching Command Line Interface (CLI)...")
    args = [cli_executable]
    if file_to_open:
        args.append(file_to_open)
    
    try:
        subprocess.run(args)
    except Exception as e:
        print(f"Failed to launch CLI editor: {e}")
        sys.exit(1)

def main():
    print("="*50)
    print("    Unified Text Editor (CLI & GUI)    ")
    print("    System Programming Lab - Assignment 3")
    print("="*50)
    
    parser = argparse.ArgumentParser(description="Unified Text Editor")
    parser.add_argument("--cli", action="store_true", help="Launch the CLI editor")
    parser.add_argument("--gui", action="store_true", help="Launch the GUI editor")
    parser.add_argument("file", nargs="?", help="File to open (CLI mode supports this currently)")
    args = parser.parse_args()

    if args.cli:
        launch_cli(args.file)
        return
    elif args.gui:
        launch_gui()
        return

    print("Please choose the interface you want to use:")
    print("1. Command Line Interface (CLI) - Terminal based")
    print("2. Graphical User Interface (GUI) - Window based")
    print("3. Exit")
    
    while True:
        try:
            choice = input("\nEnter your choice (1/2/3): ").strip()
            if choice == '1':
                file_input = input("Enter filename to open (or press Enter to create a new file): ").strip()
                if not file_input:
                    file_input = None
                launch_cli(file_input)
                break
            elif choice == '2':
                launch_gui()
                break
            elif choice == '3':
                print("Exiting...")
                sys.exit(0)
            else:
                print("Invalid choice. Please enter 1, 2, or 3.")
        except KeyboardInterrupt:
            print("\nExiting...")
            sys.exit(0)
        except EOFError:
            print("\nExiting...")
            sys.exit(0)

if __name__ == "__main__":
    main()
