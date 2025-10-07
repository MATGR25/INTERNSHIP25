import cv2
import numpy as np
import os
import json
import base64
from flask import Flask, request, jsonify

# Import the models. Ensure these are correctly installed by pip.
from keras_facenet import FaceNet
from mtcnn.mtcnn import MTCNN

# --- Configuration ---
DATABASE_FILE = "face_database.npz"
RECOGNITION_THRESHOLD = 0.9 # You can tweak this value (smaller for stricter, larger for more lenient)

# --- Initialize Flask App ---
app = Flask(__name__)

# --- Initialize Models Globally (This runs once when the script starts) ---
facenet_embedder = None
mtcnn_detector = None

try:
    print("Loading FaceNet embedder...")
    facenet_embedder = FaceNet()
    print("FaceNet embedder loaded.")

    print("Loading MTCNN detector...")
    mtcnn_detector = MTCNN()
    print("MTCNN detector loaded.")
except Exception as e:
    print(f"Error loading models: {e}")
    # In a production environment, you might want to log this and exit
    # For development, just print the error. The app might not function correctly.

# --- Helper Functions ---

def load_database():
    """
    Loads the face database from the .npz file.
    Returns an empty dictionary if the file doesn't exist.
    """
    if os.path.exists(DATABASE_FILE):
        # Correctly load the NpzFile object and convert it to a dictionary
        return dict(np.load(DATABASE_FILE, allow_pickle=True))
    return {}

def save_database(data):
    """
    Saves the current face database to the .npz file.
    """
    np.savez(DATABASE_FILE, **data)

def get_face_embedding(image_data):
    """
    Detects faces in an image using MTCNN and extracts the embedding
    of the first detected face using FaceNet.
    Args:
        image_data (numpy.ndarray): The image in RGB format.
    Returns:
        numpy.ndarray or None: The 512-dimensional face embedding, or None if no face is detected.
    """
    # Ensure models are loaded before use
    if facenet_embedder is None or mtcnn_detector is None:
        print("Models not loaded. Cannot process request.")
        return None

    # Detect faces with MTCNN
    faces = mtcnn_detector.detect_faces(image_data)

    if not faces:
        return None # No face detected

    # Get the bounding box of the first detected face
    face_info = faces[0]
    x, y, w, h = face_info['box']

    # Ensure coordinates are within image bounds to prevent errors
    h_img, w_img, _ = image_data.shape
    x, y = max(0, x), max(0, y)
    w, h = min(w, w_img - x), min(h, h_img - y)

    # Crop the face region
    face_img = image_data[y:y+h, x:x+w]

    # Get the embedding from FaceNet.
    embeddings = facenet_embedder.embeddings([face_img])
    
    return embeddings[0] # Return the first (and only) embedding

# --- API Endpoints ---

@app.route('/')
def home():
    """
    A simple home page to confirm the server is running.
    """
    return "<h1>Face Recognition API is running!</h1><p>Send POST requests to /add_person or /recognize_person.</p>"

@app.route('/add_person', methods=['POST'])
def add_person_endpoint():
    """
    Endpoint to add a new person's face or additional photos for an existing person.
    Expects JSON with 'name' (string in 'car_id/user_id' format) and 'photos' (list of base64 image strings).
    If a person exists, it averages the new embeddings with the existing one.
    Implements single unique person (by face) per car ID restriction.
    """
    data = request.get_json()
    if not data or 'name' not in data or 'photos' not in data:
        return jsonify({"status": "error", "message": "Missing 'name' or 'photos' in request."}), 400

    full_person_name = data['name'].strip()
    photos_b64 = data['photos']

    if '/' not in full_person_name:
        return jsonify({"status": "error", "message": "Invalid name format. Expected 'car_id/user_id'."}), 400
    
    car_id, user_id = full_person_name.split('/', 1)
    if not car_id or not user_id:
        return jsonify({"status": "error", "message": "Car ID or User ID cannot be empty."}), 400

    if not photos_b64:
        return jsonify({"status": "error", "message": "No photos provided."}), 400

    new_embeddings = []
    for photo_b64 in photos_b64:
        try:
            img_bytes = base64.b64decode(photo_b64)
            np_arr = np.frombuffer(img_bytes, np.uint8)
            img = cv2.imdecode(np_arr, cv2.IMREAD_COLOR)

            if img is None:
                print(f"Skipping invalid image in batch for {full_person_name}.")
                continue

            img_rgb = cv2.cvtColor(img, cv2.COLOR_BGR2RGB)
            embedding = get_face_embedding(img_rgb)

            if embedding is not None:
                new_embeddings.append(embedding)
            else:
                print(f"No face detected in one of the provided photos for {full_person_name}.")

        except Exception as e:
            print(f"Error processing a photo for {full_person_name}: {e}")
            return jsonify({"status": "error", "message": f"Error processing one of the photos: {e}"}), 400

    if not new_embeddings:
        return jsonify({"status": "error", "message": "No valid faces were detected in any of the provided photos."}), 400
    
    # Use the first new embedding for comparison with existing users in the same car.
    # If multiple photos are provided, they are for this same person.
    incoming_embedding_for_comparison = new_embeddings[0]

    database = load_database()

    if full_person_name in database:
        # Case 1: Person (car_id/user_id) already exists, so update their profile
        existing_embedding = database[full_person_name]
        combined_embeddings = [existing_embedding] + new_embeddings
        avg_embedding = np.mean(combined_embeddings, axis=0)
        message = (f"Updated {full_person_name} with {len(new_embeddings)} new photo(s), "
                   f"resulting in an average embedding. Total virtual samples: {len(combined_embeddings)}.")
    else:
        # Case 2: New car_id/user_id combination.
        # First, check if this face is already registered under a different user_id for the same car_id.
        for stored_full_name, stored_embedding in database.items():
            stored_car_id = stored_full_name.split('/')[0]
            if stored_car_id == car_id and stored_full_name != full_person_name:
                # This car_id already has a different user registered. Now check if the faces match.
                distance = np.linalg.norm(incoming_embedding_for_comparison - stored_embedding)
                if distance < RECOGNITION_THRESHOLD:
                    return jsonify({
                        "status": "error",
                        "message": f"This face is already registered for Car ID '{car_id}' under user '{stored_full_name}'. "
                                   f"Please use '{stored_full_name}' to add more photos for this car, "
                                   f"or use a new Car ID for a new person."
                    }), 400
        
        # If we reached here, it's a truly new person (new face) for this car_id
        # or it's a new car_id entirely.
        avg_embedding = np.mean(new_embeddings, axis=0)
        message = f"Added new person: {full_person_name} with an average of {len(new_embeddings)} photo(s)."
    
    database[full_person_name] = avg_embedding
    save_database(database)

    return jsonify({"status": "success", "message": message, "person_name": full_person_name}), 200


@app.route('/recognize_person', methods=['POST'])
def recognize_person_endpoint():
    """
    Endpoint to recognize a person from a photo.
    Expects JSON with 'photo' (a single base64 image string).
    Returns the recognized person's name and certainty, or 'Unknown'.
    """
    data = request.get_json()
    if not data or 'photo' not in data:
        return jsonify({"status": "error", "message": "Missing 'photo' in request."}), 400

    photo_b64 = data['photo']

    try:
        img_bytes = base64.b64decode(photo_b64)
        np_arr = np.frombuffer(img_bytes, np.uint8)
        img = cv2.imdecode(np_arr, cv2.IMREAD_COLOR)

        if img is None:
            return jsonify({"status": "error", "message": "Could not decode image. Invalid format?"}), 400

        img_rgb = cv2.cvtColor(img, cv2.COLOR_BGR2RGB)
        new_embedding = get_face_embedding(img_rgb)

        if new_embedding is None:
            return jsonify({"status": "no_face_detected", "message": "No face detected in the provided photo."}), 200

        database = load_database()
        if not database:
            return jsonify({"status": "no_database", "message": "No registered faces in the database. Add persons first."}), 200

        min_dist = float('inf')
        recognized_person_label = "Unknown" # Initialize as Unknown
        
        for name, saved_embedding in database.items():
            distance = np.linalg.norm(new_embedding - saved_embedding)
            if distance < min_dist:
                min_dist = distance
                recognized_person_label = name # Update with actual name if a closer match is found
        
        # Determine certainty and final status
        if min_dist <= RECOGNITION_THRESHOLD:
            certainty = (1.0 - (min_dist / RECOGNITION_THRESHOLD)) * 100
            status_message = f"Recognized person: {recognized_person_label}"
            return jsonify({
                "status": "recognized",
                "person_name": recognized_person_label, # The identified person
                "certainty_percentage": f"{certainty:.2f}%",
                "distance": float(min_dist)
            }), 200
        else:
            # Even if not recognized, report 'Unknown' with info about closest match if desired
            # The certainty here will be low, indicating it's outside the threshold
            certainty = (1.0 - (min_dist / (min_dist + 1e-6))) * 100 # Adjust for distances > threshold
            return jsonify({
                "status": "not_recognized",
                "person_name": "Unknown", # Explicitly send "Unknown"
                "message": f"No person recognized within threshold. Closest was {recognized_person_label}.",
                "certainty_percentage": f"{certainty:.2f}%",
                "distance": float(min_dist)
            }), 200

    except Exception as e:
        print(f"Error during recognition: {e}")
        return jsonify({"status": "error", "message": f"An error occurred during recognition: {e}"}), 500

# --- Run the Flask App ---
if __name__ == '__main__':
    app.run(debug=True)
