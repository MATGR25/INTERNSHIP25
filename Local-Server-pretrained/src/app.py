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
PHOTO_COUNTS_REPORT_FILE = "photo_counts_report.txt"
RECOGNITION_THRESHOLD = 0.75 # Set for stricter face distinction
MAX_USERS_PER_CAR_LIMIT = 5 # Configurable limit for users per car ID

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
    The stored format for each person is (average_embedding, photo_count).
    """
    if os.path.exists(DATABASE_FILE):
        loaded_data = np.load(DATABASE_FILE, allow_pickle=True)
        database = {}
        for key in loaded_data.files:
            embedding_and_count = loaded_data[key]
            database[key] = (embedding_and_count[0], int(embedding_and_count[1]))
        return database
    return {}

def generate_and_save_report(data):
    """
    Generates a text report of photo counts and vector features per person
    and saves it to a file. Called automatically when the database is saved.
    """
    report_lines = ["--- Face Database Report ---"]
    if not data:
        report_lines.append("No persons registered in the database.")
    else:
        # Sort by person name for consistent output
        sorted_names = sorted(data.keys())
        for name in sorted_names:
            embedding, count = data[name]
            report_lines.append(f"\nPerson: {name}")
            report_lines.append(f"  Photos: {count}")
            
            # Format the embedding for readability. numpy.array_str handles line wrapping.
            # precision controls decimal places, max_line_width controls wrapping.
            embedding_str = np.array_str(embedding, precision=4, max_line_width=80, suppress_small=True)
            report_lines.append(f"  Embedding (Avg):\n{embedding_str}")
            report_lines.append("---") # Separator for each person
    
    report_content = "\n".join(report_lines)
    
    try:
        with open(PHOTO_COUNTS_REPORT_FILE, "w") as f:
            f.write(report_content)
        print(f"Flask: Photo count and embedding report updated at {PHOTO_COUNTS_REPORT_FILE}")
    except Exception as e:
        print(f"Flask: Error generating photo count and embedding report: {e}")


def save_database(data):
    """
    Saves the current face database to the .npz file.
    The input 'data' dictionary should contain (average_embedding, photo_count) tuples.
    These are saved as object arrays within the npz.
    """
    data_to_save = {name: np.array([embedding, count], dtype=object) for name, (embedding, count) in data.items()}
    np.savez(DATABASE_FILE, **data_to_save)
    generate_and_save_report(data) # Call the report generation after saving

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
    Expects JSON with 'name' (string: 'car_id' for new assignment)
    and 'photos' (list of base64 image strings).

    Behavior:
    1. If 'name' is 'car_id':
       Checks if incoming face matches any existing user under 'car_id'.
       If match: This photo is ADDED to the matched user's profile.
       If no match: Assigns next available user_id (user01, user02 etc.) for that car_id, adds new user.
       If car limit reached: Returns error.
    """
    data = request.get_json()
    if not data or 'name' not in data or 'photos' not in data:
        print(f"Flask: Missing 'name' or 'photos' in request from client: {data}") # Diagnostic
        return jsonify({"status": "error", "message": "Missing 'name' or 'photos' in request."}), 400

    full_person_name_or_car_id = data['name'].strip()
    # Diagnostic print to check what name Flask actually receives
    print(f"Flask: Received name field: '{full_person_name_or_car_id}' (Type: {type(full_person_name_or_car_id)})")

    photos_b64 = data['photos']

    if not photos_b64:
        print("Flask: No photos provided in request.") # Diagnostic
        return jsonify({"status": "error", "message": "No photos provided."}), 400

    new_embeddings = []
    for photo_b64 in photos_b64:
        try:
            img_bytes = base64.b64decode(photo_b64)
            np_arr = np.frombuffer(img_bytes, np.uint8)
            img = cv2.imdecode(np_arr, cv2.IMREAD_COLOR)

            if img is None:
                print(f"Flask: Skipping invalid image in batch for {full_person_name_or_car_id}.") # Diagnostic
                continue

            img_rgb = cv2.cvtColor(img, cv2.COLOR_BGR2RGB)
            embedding = get_face_embedding(img_rgb)

            if embedding is not None:
                new_embeddings.append(embedding)
            else:
                print(f"Flask: No face detected in one of the provided photos for {full_person_name_or_car_id}.") # Diagnostic

        except Exception as e:
            print(f"Flask: Error processing a photo for {full_person_name_or_car_id}: {e}") # Diagnostic
            return jsonify({"status": "error", "message": f"Error processing one of the photos: {e}"}), 400

    if not new_embeddings:
        print("Flask: No valid faces were detected in any of the provided photos.") # Diagnostic
        return jsonify({"status": "error", "message": "No valid faces were detected in any of the provided photos."}), 400
    
    incoming_embedding = new_embeddings[0] # Use first new embedding for comparison

    database = load_database()
    
    # Flask always expects just 'car_id' from now on for 'add_person'
    car_id = full_person_name_or_car_id
    # Diagnostic print
    print(f"Flask: Processing 'add_person' for Car ID: '{car_id}'")

    # Check if the face matches any existing user under this car_id
    current_users_for_car = []
    matched_user_name = None # To store the name of the matched user, if any
    for stored_full_name, (stored_embedding, _) in database.items():
        stored_car_id = stored_full_name.split('/')[0]
        if stored_car_id == car_id:
            current_users_for_car.append(stored_full_name)
            distance = np.linalg.norm(incoming_embedding - stored_embedding)
            if distance < RECOGNITION_THRESHOLD:
                # Face matches an existing user in this car_id
                print(f"Flask: Incoming face matches existing user '{stored_full_name}' in car '{car_id}'.") # Diagnostic
                matched_user_name = stored_full_name
                break # Found a match, no need to check others in this car
        
        # This conditional block handles a specific legacy client behavior where client sends 'car_id/user_id'
        # With current ESP32, this 'if' block won't be entered for 'add' requests as ESP32 only sends 'car_id'.
        # However, it's kept for robustness or if you have other clients.
        if '/' in full_person_name_or_car_id: # This branch is for legacy client behavior
             if full_person_name_or_car_id in database:
                 # This part handles updating existing users based on client provided full name (e.g., 'car123/user01')
                 existing_embedding, existing_photo_count = database[full_person_name_or_car_id]
                 combined_embeddings_for_avg = [existing_embedding] + new_embeddings
                 avg_embedding = np.mean(combined_embeddings_for_avg, axis=0)
                 new_total_photo_count = existing_photo_count + len(new_embeddings)
                 database[full_person_name_or_car_id] = (avg_embedding, new_total_photo_count)
                 save_database(database) # This now triggers report generation
                 message = (f"Updated {full_person_name_or_car_id} with {len(new_embeddings)} new photo(s), "
                            f"now has a total of {new_total_photo_count} photos.")
                 return jsonify({"status": "success", "message": message, "person_name": full_person_name_or_car_id}), 200
             else:
                 # This case means a specific full_person_name was sent but it doesn't exist and didn't match
                 # an existing user within the car_id. This is an error state for this specific request type.
                 return jsonify({
                     "status": "error",
                     "message": f"The specific user '{full_person_name_or_car_id}' does not exist for Car ID '{car_id}'. "
                                "If adding a new user, send just 'car_id' in the 'name' field."
                 }), 400

    if matched_user_name:
        # *** Logic for automatically updating the matched user's profile ***
        existing_embedding, existing_photo_count = database[matched_user_name]
        combined_embeddings_for_avg = [existing_embedding] + new_embeddings
        avg_embedding = np.mean(combined_embeddings_for_avg, axis=0)
        new_total_photo_count = existing_photo_count + len(new_embeddings)
        database[matched_user_name] = (avg_embedding, new_total_photo_count)
        save_database(database) # This now triggers report generation

        message = (f"Updated existing user: {matched_user_name} with {len(new_embeddings)} new photo(s), "
                   f"now has a total of {new_total_photo_count} photos.")
        return jsonify({
            "status": "success", # Status is now success!
            "message": message,
            "person_name": matched_user_name, # The name of the updated person
            "assigned_user": matched_user_name # Indicate the user that was updated
        }), 200
        
    # If no face match AND no existing user update, attempt to add a *new* user
    if len(current_users_for_car) >= MAX_USERS_PER_CAR_LIMIT:
        print(f"Flask: Car ID '{car_id}' limit reached ({MAX_USERS_PER_CAR_LIMIT}).") # Diagnostic
        return jsonify({
            "status": "error",
            "message": f"Car ID '{car_id}' has reached its maximum user limit of {MAX_USERS_PER_CAR_LIMIT}. Cannot add new users."
        }), 400
    
    # Find next available user_id (e.g., user01, user02, etc.)
    existing_user_numbers = []
    for stored_full_name in current_users_for_car:
        try:
            # Extract the user number (e.g., '01' from 'car123/user01')
            user_num_str = stored_full_name.split('/')[1][4:] # Remove 'user' prefix
            existing_user_numbers.append(int(user_num_str))
        except (IndexError, ValueError):
            print(f"Flask: Warning - Malformed user name found in database: {stored_full_name}") # Diagnostic
            continue # Ignore malformed user names
    
    next_user_num = 1
    while next_user_num in existing_user_numbers:
        next_user_num += 1
    
    new_user_id = f"user{next_user_num:02d}" # Format as user01, user02 etc.
    assigned_full_name = f"{car_id}/{new_user_id}" # This uses the 'car_id' from the request
    
    # Diagnostic print for assignment
    print(f"Flask: Assigning new user '{assigned_full_name}' for car '{car_id}'.")

    avg_embedding = np.mean(new_embeddings, axis=0)
    database[assigned_full_name] = (avg_embedding, len(new_embeddings))
    save_database(database) # This now triggers report generation
    message = (f"Added new person: {assigned_full_name} with {len(new_embeddings)} photo(s).")
    return jsonify({
        "status": "success",
        "message": message,
        "person_name": assigned_full_name,
        "assigned_user": assigned_full_name # Indicate the new user ID
    }), 200


@app.route('/recognize_person', methods=['POST'])
def recognize_person_endpoint():
    """
    Endpoint to recognize a person from a photo.
    Expects JSON with 'photo' (a single base64 image string).
    Returns the recognized person's name and certainty, or 'Unknown'.
    """
    data = request.get_json()
    if not data or 'photo' not in data:
        print(f"Flask: Missing 'photo' in recognize request: {data}") # Diagnostic
        return jsonify({"status": "error", "message": "Missing 'photo' in request."}), 400

    photo_b64 = data['photo']

    try:
        img_bytes = base64.b64decode(photo_b64)
        np_arr = np.frombuffer(img_bytes, np.uint8)
        img = cv2.imdecode(np_arr, cv2.IMREAD_COLOR)

        if img is None:
            print("Flask: Could not decode image in recognize request.") # Diagnostic
            return jsonify({"status": "error", "message": "Could not decode image. Invalid format?"}), 400

        img_rgb = cv2.cvtColor(img, cv2.COLOR_BGR2RGB)
        new_embedding = get_face_embedding(img_rgb)

        if new_embedding is None:
            print("Flask: No face detected in recognize photo.") # Diagnostic
            return jsonify({"status": "no_face_detected", "message": "No face detected in the provided photo."}), 200

        database = load_database()
        if not database:
            print("Flask: No registered faces in database for recognition.") # Diagnostic
            return jsonify({"status": "no_database", "message": "No registered faces in the database. Add persons first."}), 200

        min_dist = float('inf')
        recognized_person_label = "Unknown" # Initialize as Unknown
        
        for name, (saved_embedding, _) in database.items(): # Unpack the tuple here
            distance = np.linalg.norm(new_embedding - saved_embedding)
            if distance < min_dist:
                min_dist = distance
                recognized_person_label = name # Update with actual name if a closer match is found
        
        if min_dist <= RECOGNITION_THRESHOLD:
            certainty = (1.0 - (min_dist / RECOGNITION_THRESHOLD)) * 100
            status_message = f"Recognized person: {recognized_person_label}"
            print(f"Flask: Recognized '{recognized_person_label}' with {certainty:.2f}% certainty.") # Diagnostic
            return jsonify({
                "status": "recognized",
                "person_name": recognized_person_label,
                "certainty_percentage": f"{certainty:.2f}%",
                "distance": float(min_dist)
            }), 200
        else:
            certainty = (1.0 - (min_dist / (min_dist + 1e-6))) * 100
            print(f"Flask: Not recognized. Closest was '{recognized_person_label}' with {certainty:.2f}% certainty.") # Diagnostic
            return jsonify({
                "status": "not_recognized",
                "person_name": "Unknown",
                "message": f"No person recognized within threshold. Closest was {recognized_person_label}.",
                "certainty_percentage": f"{certainty:.2f}%",
                "distance": float(min_dist)
            }), 200

    except Exception as e:
        print(f"Flask: Error during recognition: {e}") # Diagnostic
        return jsonify({"status": "error", "message": f"An error occurred during recognition: {e}"}), 500

@app.route('/generate_photo_counts_report', methods=['GET'])
def generate_photo_counts_report():
    """
    Endpoint to generate a text file report of photo counts per person.
    This can be called manually via a GET request if needed.
    """
    database = load_database()
    generate_and_save_report(database) # This function now handles saving the report
    return jsonify({
        "status": "success",
        "message": f"Photo count report generated and saved to {PHOTO_COUNTS_REPORT_FILE}."
    }), 200


# --- Run the Flask App ---
if __name__ == '__main__':
    # Initial report generation when the app starts
    generate_and_save_report(load_database())
    app.run(host='0.0.0.0', debug=True)
