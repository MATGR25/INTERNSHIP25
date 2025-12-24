import requests
import base64
import os

# --- Configuration ---
FLASK_APP_URL = "http://127.0.0.1:5000"

def encode_image_to_base64(image_path):
    """
    Reads an image file and encodes it to a base64 string.
    """
    if not os.path.exists(image_path):
        print(f"Error: Image file not found at {image_path}")
        return None
    with open(image_path, "rb") as image_file:
        encoded_string = base64.b64encode(image_file.read()).decode('utf-8')
    return encoded_string

def add_person_request(full_person_name, photo_paths):
    """
    Sends a POST request to the /add_person endpoint.
    `full_person_name` should be in 'car_id/user_id' format.
    Can send multiple photos at once for averaging.
    """
    print(f"\n--- Adding Person: {full_person_name} ---")
    photos_b64 = []
    for path in photo_paths:
        encoded_photo = encode_image_to_base64(path)
        if encoded_photo:
            photos_b64.append(encoded_photo)
    
    if not photos_b64:
        print("No valid photos to add.")
        return

    payload = {
        "name": full_person_name,
        "photos": photos_b64
    }
    
    try:
        response = requests.post(f"{FLASK_APP_URL}/add_person", json=payload)
        response.raise_for_status() # Raise HTTPError for bad responses (4xx or 5xx)
        print("Response:", response.json())
    except requests.exceptions.RequestException as e:
        print(f"Error sending add_person request: {e}")
        if hasattr(e, 'response') and e.response is not None:
            print("Server Response (Error):", e.response.json())


def recognize_person_request(photo_path):
    """
    Sends a POST request to the /recognize_person endpoint.
    """
    print(f"\n--- Recognizing Person from: {photo_path} ---")
    encoded_photo = encode_image_to_base64(photo_path)
    if not encoded_photo:
        return

    payload = {
        "photo": encoded_photo
    }

    try:
        response = requests.post(f"{FLASK_APP_URL}/recognize_person", json=payload)
        response.raise_for_status()
        print("Response:", response.json())
    except requests.exceptions.RequestException as e:
        print(f"Error sending recognize_person request: {e}")
        if hasattr(e, 'response') and e.response is not None:
            print("Server Response (Error):", e.response.json())


if __name__ == "__main__":
    
    # data base is saved incremnetly so be ware of duplicating

    # if you want to add person like name mohamed add_person_request("mohamed", ["Path"])

    #   ex:  add_person_request("car123/user06", ["./test_images/photo_7 (2).jpg"])

    # if you want to recognize  recognize_person_request("Path")

    #  ex :recognize_person_request("./test_images/testme.jpg")
