#!/usr/bin/env python3
import csv
import sys

def parse_size(size_str):
    """Convert size like '320K', '1M', or '0x1000' to bytes."""
    size_str = size_str.strip().upper()
    if size_str.endswith('K'):
        return int(size_str[:-1], 0) * 1024
    elif size_str.endswith('M'):
        return int(size_str[:-1], 0) * 1024 * 1024
    return int(size_str, 0)

def find_partition_offset(csv_file, target_name):
    computed_offset = None
    first_partition = True

    with open(csv_file, newline='') as f:
        reader = csv.reader(f)
        for row in reader:
            if not row or row[0].strip().startswith('#'):
                continue

            fields = [cell.strip() for cell in row]
            if len(fields) < 5:
                raise ValueError(f"Malformed row: {row}")

            name, _, _, raw_offset, size_str = fields[:5]
            size = parse_size(size_str)

            if first_partition:
                if not raw_offset:
                    raise ValueError("First partition must have an explicit offset.")
                computed_offset = int(raw_offset, 0)
                first_partition = False
            else:
                if raw_offset:
                    explicit_offset = int(raw_offset, 0)
                    if explicit_offset != computed_offset:
                        raise ValueError(
                            f"Offset mismatch for partition '{name}': "
                            f"expected 0x{computed_offset:X}, got 0x{explicit_offset:X}"
                        )

            if name == target_name:
                return hex(computed_offset)

            computed_offset += size

    raise ValueError(f"Partition '{target_name}' not found.")

if __name__ == "__main__":
    if len(sys.argv) != 3:
        print("Get partition offset from a partitions.csv file\nUsage: partoffs.py <partition-file> <partition_name>", file=sys.stderr)
        sys.exit(1)

    csv_file = sys.argv[1]
    partition_name = sys.argv[2]

    try:
        offset = find_partition_offset(csv_file, partition_name)
        print(offset)
    except Exception as e:
        print(f"Error: {e}", file=sys.stderr)
        sys.exit(1)
