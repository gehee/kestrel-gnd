import sys

from PIL import Image

def remove_white_background(input_path, output_path):
    img = Image.open(input_path)
    img = img.convert("RGBA")
    datas = img.getdata()

    new_data = []
    for item in datas:
        # Check if the pixel is "light" (background)
        if item[0] > 200 and item[1] > 200 and item[2] > 200:
            # Make background transparent
            new_data.append((255, 255, 255, 0))
        else:
            # Make logo content (darker pixels) PURE WHITE
            new_data.append((255, 255, 255, 255))

    img.putdata(new_data)
    img.save(output_path, "PNG")
    print(f"Saved transparent logo to {output_path}")

if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit("usage: remove_bg.py <input.png> <output.png>")
    remove_white_background(sys.argv[1], sys.argv[2])
