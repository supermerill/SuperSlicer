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

# Function to replace tabs with 4 spaces in a file and count replacements
replace_tabs_in_file() {
    local file=$1
    local replacements=0

    # Check if the file is a text file
    if file "$file" | grep -q "text"; then
        # Count the number of tab characters
        local count=$(grep -o $'\t' "$file" | wc -l)
        # Replace tabs with 4 spaces
        sed -i 's/\t/    /g' "$file"
        # Add the count to replacements
        replacements=$((replacements + count))
    fi

    echo $replacements
}

# Total replacements counter
total_replacements=0

# Export the function to be used by find -exec
export -f replace_tabs_in_file

# Export the total_replacements variable to be updated by subshells
export total_replacements

# Process all files in the directory recursively
total_replacements=$(find "$directory" -type f -exec bash -c '
    for file; do
        replacements=$(replace_tabs_in_file "$file")
        total_replacements=$((total_replacements + replacements))
    done
    echo $total_replacements
' bash {} +)

# Output the total number of replacements
echo "Total number of replacements: $total_replacements"
