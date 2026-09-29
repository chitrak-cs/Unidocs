# Interactive To-Do List Manager

todo_list = []

def display_menu():
    print("\n--- TO-DO LIST ---")
    print("1. View tasks")
    print("2. Add task")
    print("3. Remove task")
    print("4. Exit")

while True:
    display_menu()
    choice = input("\nChoose an option (1-4): ").strip()

    if choice == "1":
        if not todo_list:
            print("Your list is currently empty!")
        else:
            print("\nYour Tasks:")
            for index, task in enumerate(todo_list, start=1):
                print(f"  {index}. {task}")

    elif choice == "2":
        task = input("Enter a task: ").strip()
        if task:
            todo_list.append(task)
            print(f"Added: '{task}'")
        else:
            print("Task cannot be empty.")

    elif choice == "3":
        if not todo_list:
            print("No tasks available to remove.")
        else:
            try:
                task_num = int(input("Enter task number to remove: "))
                removed_task = todo_list.pop(task_num - 1)
                print(f"Removed: '{removed_task}'")
            except (IndexError, ValueError):
                print("Invalid task number.")

    elif choice == "4":
        print("Goodbye!")
        break

    else:
        print("Invalid choice. Please pick 1, 2, 3, or 4.")# Interactive To-Do List Manager

todo_list = []

def display_menu():
    print("\n--- TO-DO LIST ---")
    print("1. View tasks")
    print("2. Add task")
    print("3. Remove task")
    print("4. Exit")

while True:
    display_menu()
    choice = input("\nChoose an option (1-4): ").strip()

    if choice == "1":
        if not todo_list:
            print("Your list is currently empty!")
        else:
            print("\nYour Tasks:")
            for index, task in enumerate(todo_list, start=1):
                print(f"  {index}. {task}")

    elif choice == "2":
        task = input("Enter a task: ").strip()
        if task:
            todo_list.append(task)
            print(f"Added: '{task}'")
        else:
            print("Task cannot be empty.")

    elif choice == "3":
        if not todo_list:
            print("No tasks available to remove.")
        else:
            try:
                task_num = int(input("Enter task number to remove: "))
                removed_task = todo_list.pop(task_num - 1)
                print(f"Removed: '{removed_task}'")
            except (IndexError, ValueError):
                print("Invalid task number.")

    elif choice == "4":
        print("Goodbye!")
        break

    else:
        print("Invalid choice. Please pick 1, 2, 3, or 4.")