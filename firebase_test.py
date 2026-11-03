import firebase_admin
from firebase_admin import credentials, db
from datetime import datetime, timezone
import os

# Load Firebase service account
cred = credentials.Certificate(
    os.environ["GOOGLE_APPLICATION_CREDENTIALS"]
)

# Initialize Firebase
firebase_admin.initialize_app(
    cred,
    {
        "databaseURL": "https://ai-water-quality-monitor-f6dc4-default-rtdb.asia-southeast1.firebasedatabase.app"
    }
)

# Test water-quality data
data = {
    "pH": 7.38,
    "TDS": 138.7,
    "Turbidity": 0.25,
    "Temperature": 20.25,
    "prediction": "Drinkable",
    "timestamp": datetime.now(timezone.utc).isoformat()
}

# Write test data
db.reference("latest").set(data)

print("Firebase data uploaded successfully!")