import os
import cv2
import numpy as np
import random
from PIL import Image, ImageEnhance
import matplotlib.pyplot as plt
from mtcnn.mtcnn import MTCNN

# --- Configuration Variables ---
AUGMENTATION_COUNT_PER_IMAGE = 50  # Generate 30 augmented versions
SAVE_AUGMENTED_IMAGES = True  # Set to True to save images
OUTPUT_DIRECTORY = "./augmented_photos"  # Directory to save augmented images

# Config paramters
FLIP_HORIZONTAL_PROB = 0.7
BRIGHTNESS_RANGE = (0.6, 1.4)
CONTRAST_RANGE = (0.8, 1.5)
SATURATION_RANGE = (0.5, 1.4)
SHARPNESS_RANGE = (0.5, 2)
ROTATION_ANGLES = [ -20, -15, -10, -5, 0 , 5, 10, 15, 20]
SKEW_OPTIONS = [(0, 0), (-0.15, 0), (0.15, 0), (0, -0.15), (0, 0.15), 
                (-0.1, -0.1), (0.1, 0.1)]
NOISE_PROBABILITY = 0.3
NOISE_INTENSITY_RANGE = (5, 10)

# FaceNet's required input size
REQUIRED_FACE_SIZE = (160, 160)

# --- Initialize MTCNN for Face Cropping ---
mtcnn_detector = MTCNN()

# --- Create output directory if it doesn't exist ---
if SAVE_AUGMENTED_IMAGES and not os.path.exists(OUTPUT_DIRECTORY):
    os.makedirs(OUTPUT_DIRECTORY)
    print(f"Created output directory: {OUTPUT_DIRECTORY}")

# --- Helper Function to Preprocess/Crop a Single Image ---
def get_single_cropped_face(image_path):
    img = cv2.imread(image_path)
    if img is None:
        print(f"Error: Could not read image at {image_path}. Please check the path.")
        return None
    
    img_rgb = cv2.cvtColor(img, cv2.COLOR_BGR2RGB)
    faces = mtcnn_detector.detect_faces(img_rgb)

    cropped_face = None
    if faces:
        x, y, w, h = faces[0]['box']
        
        # Expand bounding box slightly for better cropping
        margin_px = 80
        x1, y1 = max(0, x - margin_px), max(0, y - margin_px)
        x2, y2 = min(img_rgb.shape[1], x + w + margin_px), min(img_rgb.shape[0], y + h + margin_px)
        
        cropped_face = img_rgb[y1:y2, x1:x2]
    else:
        print(f"No face detected in {image_path}. Using resized original image as fallback.")
        cropped_face = img_rgb

    # Resize to FaceNet's input size
    cropped_face = cv2.resize(cropped_face, REQUIRED_FACE_SIZE, interpolation=cv2.INTER_AREA)
    return cropped_face

# --- Enhanced Augmentation Function ---
def augment_cropped_face_enhanced(image_array):
    img_pil = Image.fromarray(image_array)

    # 1. FLIPPING (More likely)
    if random.random() < FLIP_HORIZONTAL_PROB:
        img_pil = img_pil.transpose(Image.FLIP_LEFT_RIGHT)

    # 2. COLOR JITTER (More extreme ranges)
    # Brightness
    brightness_factor = random.uniform(BRIGHTNESS_RANGE[0], BRIGHTNESS_RANGE[1])
    enhancer = ImageEnhance.Brightness(img_pil)
    img_pil = enhancer.enhance(brightness_factor)

    # Contrast
    contrast_factor = random.uniform(CONTRAST_RANGE[0], CONTRAST_RANGE[1])
    enhancer = ImageEnhance.Contrast(img_pil)
    img_pil = enhancer.enhance(contrast_factor)

    # Color Saturation (NEW)
    saturation_factor = random.uniform(SATURATION_RANGE[0], SATURATION_RANGE[1])
    enhancer = ImageEnhance.Color(img_pil)
    img_pil = enhancer.enhance(saturation_factor)

    # Sharpness (NEW)
    sharpness_factor = random.uniform(SHARPNESS_RANGE[0], SHARPNESS_RANGE[1])
    enhancer = ImageEnhance.Sharpness(img_pil)
    img_pil = enhancer.enhance(sharpness_factor)

    # 3. ROTATION (Larger angles)
    angle = random.choice(ROTATION_ANGLES)
    img_pil = img_pil.rotate(angle, resample=Image.BICUBIC, expand=False, fillcolor=(0, 0, 0))

    # 4. SKEW / PERSPECTIVE (More extreme)
    skew_x, skew_y = random.choice(SKEW_OPTIONS)
    pil_affine_matrix = (1, -skew_x, 0, -skew_y, 1, 0)
    img_pil = img_pil.transform(img_pil.size, Image.AFFINE, pil_affine_matrix, resample=Image.BICUBIC)

    # 5. NOISE INJECTION (NEW) - Adds grain
    if random.random() < NOISE_PROBABILITY:
        img_array = np.array(img_pil).astype(np.float32)
        noise_intensity = random.randint(NOISE_INTENSITY_RANGE[0], NOISE_INTENSITY_RANGE[1])
        noise = np.random.normal(0, noise_intensity, img_array.shape).astype(np.float32)
        img_array = np.clip(img_array + noise, 0, 255)
        img_pil = Image.fromarray(img_array.astype(np.uint8))

    return np.array(img_pil)

# --- Function to save images ---
def save_augmented_images(original_face, augmented_photos, original_image_path, output_dir):
    """Save original and augmented images to the specified directory"""
    # Get the original filename without extension
    original_filename = os.path.splitext(os.path.basename(original_image_path))[0]
    
    # Save original image
    original_output_path = os.path.join(output_dir, f"{original_filename}_original.jpg")
    cv2.imwrite(original_output_path, cv2.cvtColor(original_face, cv2.COLOR_RGB2BGR))
    print(f"Saved original: {original_output_path}")
    
    # Save augmented images
    for i, aug_face in enumerate(augmented_photos):
        aug_output_path = os.path.join(output_dir, f"{original_filename}_aug_{i+1:02d}.jpg")
        cv2.imwrite(aug_output_path, cv2.cvtColor(aug_face, cv2.COLOR_RGB2BGR))
        print(f"Saved augmented {i+1:02d}: {aug_output_path}")

# --- Main Visualization Logic ---
if __name__ == "__main__":
    # --- STEP 1: Specify the input photo path ---
    input_photo_path = ".\photo_1.jpg"  # <--- EDIT THIS LINE TO YOUR IMAGE PATH

    if not os.path.exists(input_photo_path):
        print(f"Error: The specified input photo path does not exist: {input_photo_path}")
        print("Please update 'input_photo_path' to a valid image file.")
    else:
        # Get the original cropped face
        original_face = get_single_cropped_face(input_photo_path)
        
        if original_face is None:
            print("Could not process the input photo. Exiting.")
        else:
            # --- STEP 2: Generate Augmented Photos ---
            augmented_photos = []
            for i in range(AUGMENTATION_COUNT_PER_IMAGE):
                augmented_photos.append(augment_cropped_face_enhanced(original_face))
            
            # --- STEP 3: Save images if enabled ---
            if SAVE_AUGMENTED_IMAGES:
                save_augmented_images(original_face, augmented_photos, input_photo_path, OUTPUT_DIRECTORY)
                print(f"\nAll images saved to: {os.path.abspath(OUTPUT_DIRECTORY)}")
            
            # --- STEP 4: Visualize in a grid ---
            # Calculate grid size (5 columns, enough rows)
            cols = 5
            rows = (AUGMENTATION_COUNT_PER_IMAGE + 1) // cols + 1
            
            # Create figure
            plt.figure(figsize=(20, 4 * rows))
            
            # Display Original Face
            plt.subplot(rows, cols, 1)
            plt.imshow(original_face)
            plt.title("Original", fontsize=10)
            plt.axis('off')

            # Display Augmented Faces
            for i, aug_face in enumerate(augmented_photos):
                plt.subplot(rows, cols, i + 2)  # +2 because 1st is original
                plt.imshow(aug_face)
                plt.title(f"Aug {i+1}", fontsize=8)
                plt.axis('off')
            
            plt.tight_layout()
            plt.show()

            print("\nVisualization complete!")
            print("Parameters you can adjust:")
            print(f"  FLIP_HORIZONTAL_PROB: {FLIP_HORIZONTAL_PROB} (0-1)")
            print(f"  BRIGHTNESS_RANGE: {BRIGHTNESS_RANGE}")
            print(f"  CONTRAST_RANGE: {CONTRAST_RANGE}")
            print(f"  SATURATION_RANGE: {SATURATION_RANGE}")
            print(f"  SHARPNESS_RANGE: {SHARPNESS_RANGE}")
            print(f"  ROTATION_ANGLES: {ROTATION_ANGLES}")
            print(f"  SKEW_OPTIONS: {SKEW_OPTIONS}")
            print(f"  NOISE_PROBABILITY: {NOISE_PROBABILITY} (0-1)")
            print(f"  NOISE_INTENSITY_RANGE: {NOISE_INTENSITY_RANGE}")
            print(f"  OUTPUT_DIRECTORY: {OUTPUT_DIRECTORY}")
