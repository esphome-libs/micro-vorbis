#!/usr/bin/env python3
"""
Convert an Ogg Vorbis audio file to a C header file with byte array.
"""

import sys
import os
import argparse

def convert_ogg_to_header(ogg_file, header_file, array_name, description):
    """Convert ogg file to C header with byte array."""

    with open(ogg_file, 'rb') as f:
        ogg_data = f.read()

    guard_name = os.path.basename(header_file).upper().replace('.', '_')

    with open(header_file, 'w') as f:
        f.write("/* Auto-generated from {} */\n".format(os.path.basename(ogg_file)))
        f.write("/* File size: {} bytes */\n".format(len(ogg_data)))
        if description:
            f.write("/*\n")
            for line in description.strip().split('\n'):
                f.write(" * {}\n".format(line))
            f.write(" */\n")
        f.write("\n")
        f.write("#ifndef {}\n".format(guard_name))
        f.write("#define {}\n\n".format(guard_name))
        f.write("#include <stddef.h>\n")  # size_t
        f.write("#include <stdint.h>\n\n")  # uint8_t

        f.write("static const uint8_t {}[] = {{\n".format(array_name))

        for i in range(0, len(ogg_data), 16):
            chunk = ogg_data[i:i+16]
            hex_values = ', '.join('0x{:02X}'.format(b) for b in chunk)
            f.write("    {}".format(hex_values))
            if i + 16 < len(ogg_data):
                f.write(",\n")
            else:
                f.write("\n")

        f.write("};\n\n")
        f.write("static const size_t {}_size = sizeof({});\n\n".format(array_name, array_name))
        f.write("#endif // {}\n".format(guard_name))

    print(f"Converted {ogg_file} to {header_file}")
    print(f"Array name: {array_name}")
    print(f"Data size: {len(ogg_data)} bytes")

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description='Convert Ogg Vorbis file to C header')
    parser.add_argument('input', help='Input .ogg file')
    parser.add_argument('output', help='Output .h file')
    parser.add_argument('--name', '-n', default='test_vorbis_data',
                        help='Name for the data array (default: test_vorbis_data)')
    parser.add_argument('--description', '-d', default='',
                        help='Multi-line description comment for the header')
    args = parser.parse_args()

    if not os.path.exists(args.input):
        print(f"Error: Input file '{args.input}' not found")
        sys.exit(1)

    convert_ogg_to_header(args.input, args.output, args.name, args.description)
