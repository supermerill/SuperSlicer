#!/bin/bash

# Check if the directory name is provided
if [ $# -eq 0 ]; then
    echo "Usage: $0 <directory>"
    exit 1
fi

# Directory to process
directory=$1

# Check if the directory exists
if [ ! -d "$directory" ]; then
    echo "Directory not found!"
    exit 1
fi

# Function to remove trailing whitespace in a file and count changes
remove_trailing_whitespace_in_file() {
    local file=$1
    local changes=0

    # Check if the file is a text file
    if file "$file" | grep -q "text"; then
        # Remove trailing whitespace (spaces and tabs)
        sed -i 's/[[:space:]]*$//' "$file"
        # Check if changes were made
        if ! diff -q "$file" /dev/null &>/dev/null; then
            changes=1
        fi
    fi

    echo $changes
}

# Total changes counter
total_changes=0

# Export the function to be used by find -exec
export -f remove_trailing_whitespace_in_file

# Export the total_changes variable to be updated by subshells
export total_changes

# Process all files in the directory recursively
total_changes=$(find "$directory" -type f -exec bash -c '
    for file; do
        changes=$(remove_trailing_whitespace_in_file "$file")
        total_changes=$((total_changes + changes))
    done
    echo $total_changes
' bash {} +)

# Output the total number of changes made
echo "Total number of changes made: $total_changes"
