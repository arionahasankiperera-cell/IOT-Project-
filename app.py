from flask import Flask, request, jsonify, render_template
import joblib
import pandas as pd
import firebase_admin
from firebase_admin import credentials, db
from datetime import datetime, timezone
import os


# ============================================================
# FLASK APPLICATION
# ============================================================

app = Flask(__name__)


# ============================================================
# PROJECT PATHS
# ============================================================

BASE_DIR = os.path.dirname(
    os.path.abspath(__file__)
)

MODEL_PATH = os.path.join(
    BASE_DIR,
    "..",
    "ml",
    "model.pkl"
)


# ============================================================
# LOAD DECISION TREE MODEL
# ============================================================

model = joblib.load(MODEL_PATH)

print("Decision Tree model loaded successfully.")


# ============================================================
# FIREBASE CONNECTION
# ============================================================

if not firebase_admin._apps:

    cred_path = os.getenv(
        "GOOGLE_APPLICATION_CREDENTIALS"
    )

    if not cred_path:
        raise RuntimeError(
            "GOOGLE_APPLICATION_CREDENTIALS environment variable is not set."
        )

    cred = credentials.Certificate(
        cred_path
    )

    firebase_admin.initialize_app(
        cred,
        {
            "databaseURL":
                "https://ai-water-quality-monitor-f6dc4-default-rtdb.asia-southeast1.firebasedatabase.app"
        }
    )


print("Firebase connected successfully.")


# ============================================================
# HOME PAGE
# ============================================================

@app.route("/")
def home():

    return render_template(
        "index.html"
    )


# ============================================================
# SET WATER SOURCE
# ============================================================

@app.route(
    "/set-source",
    methods=["POST"]
)
def set_source():

    try:

        data = request.get_json()

        if not data:

            return jsonify({
                "error": "No source data received."
            }), 400


        source_type = str(
            data.get(
                "source_type",
                ""
            )
        ).strip()


        source_description = str(
            data.get(
                "source_description",
                ""
            )
        ).strip()


        if not source_type:

            return jsonify({
                "error": "Source type is required."
            }), 400


        # ----------------------------------------------------
        # Save CURRENT selected source only.
        #
        # This does NOT modify old history records.
        # ----------------------------------------------------

        selected_source = {

            "source_type":
                source_type,

            "source_description":
                source_description,

            "updated_at":
                datetime.now(
                    timezone.utc
                ).isoformat()

        }


        db.reference(
            "selected_source"
        ).set(
            selected_source
        )


        print(
            "Water source updated:",
            selected_source
        )


        return jsonify({

            "success": True,

            "source_type":
                source_type,

            "source_description":
                source_description

        })


    except Exception as e:

        print(
            "Source update error:",
            str(e)
        )

        return jsonify({

            "error":
                str(e)

        }), 500


# ============================================================
# READ BOTH ESP32 SENSOR NODES
# ============================================================

def get_sensor_data():

    # --------------------------------------------------------
    # ESP32 #1
    # pH + TDS
    # --------------------------------------------------------

    esp32_1 = db.reference(
        "sensors/esp32_1"
    ).get()


    # --------------------------------------------------------
    # ESP32 #2
    # Turbidity + Temperature
    # --------------------------------------------------------

    esp32_2 = db.reference(
        "sensors/esp32_2"
    ).get()


    # --------------------------------------------------------
    # Check ESP32 #1
    # --------------------------------------------------------

    if not esp32_1:

        raise ValueError(
            "ESP32 #1 data is not available in Firebase."
        )


    # --------------------------------------------------------
    # Check ESP32 #2
    # --------------------------------------------------------

    if not esp32_2:

        raise ValueError(
            "ESP32 #2 data is not available in Firebase."
        )


    # --------------------------------------------------------
    # Check required fields
    # --------------------------------------------------------

    required_esp32_1 = [
        "pH",
        "TDS"
    ]

    required_esp32_2 = [
        "Turbidity",
        "Temperature"
    ]


    for field in required_esp32_1:

        if field not in esp32_1:

            raise ValueError(
                f"ESP32 #1 field '{field}' is missing."
            )


    for field in required_esp32_2:

        if field not in esp32_2:

            raise ValueError(
                f"ESP32 #2 field '{field}' is missing."
            )


    # --------------------------------------------------------
    # Convert to numbers
    # --------------------------------------------------------

    ph = float(
        esp32_1["pH"]
    )

    tds = float(
        esp32_1["TDS"]
    )

    turbidity = float(
        esp32_2["Turbidity"]
    )

    temperature = float(
        esp32_2["Temperature"]
    )


    # --------------------------------------------------------
    # Validate numbers
    # --------------------------------------------------------

    values = {
        "pH": ph,
        "TDS": tds,
        "Turbidity": turbidity,
        "Temperature": temperature
    }


    for name, value in values.items():

        if not pd.notna(value):

            raise ValueError(
                f"Invalid {name} value received."
            )


    # --------------------------------------------------------
    # Validate temperature
    # --------------------------------------------------------

    if temperature <= -100:

        raise ValueError(
            "Invalid temperature received from ESP32 #2."
        )


    return {

        "pH":
            ph,

        "TDS":
            tds,

        "Turbidity":
            turbidity,

        "Temperature":
            temperature

    }


# ============================================================
# DECISION TREE PREDICTION
# ============================================================

def make_prediction(
    ph,
    tds,
    turbidity,
    temperature
):

    input_data = pd.DataFrame(
        [
            {

                "pH":
                    ph,

                "TDS_ppm":
                    tds,

                "Turbidity_NTU":
                    turbidity,

                "Temperature_C":
                    temperature

            }
        ]
    )


    model_prediction = model.predict(
        input_data
    )[0]


    if model_prediction == "Safe":

        result = "Drinkable"


    elif model_prediction == "Unsafe":

        result = "Non-Drinkable"


    else:

        result = "Non-Drinkable"


    return (
        model_prediction,
        result
    )


# ============================================================
# HELPER:
# COMPARE SENSOR VALUES
# ============================================================

def sensor_values_changed(
    previous,
    current
):

    if not previous:

        return True


    try:

        previous_ph = float(
            previous.get(
                "pH",
                -999999
            )
        )

        previous_tds = float(
            previous.get(
                "TDS",
                -999999
            )
        )

        previous_turbidity = float(
            previous.get(
                "Turbidity",
                -999999
            )
        )

        previous_temperature = float(
            previous.get(
                "Temperature",
                -999999
            )
        )


        current_ph = float(
            current["pH"]
        )

        current_tds = float(
            current["TDS"]
        )

        current_turbidity = float(
            current["Turbidity"]
        )

        current_temperature = float(
            current["Temperature"]
        )


        return not (

            previous_ph == current_ph

            and

            previous_tds == current_tds

            and

            previous_turbidity == current_turbidity

            and

            previous_temperature == current_temperature

        )


    except Exception:

        return True


# ============================================================
# GET CURRENT SELECTED SOURCE
# ============================================================

def get_selected_source():

    selected_source = db.reference(
        "selected_source"
    ).get()


    if selected_source:

        source_type = selected_source.get(
            "source_type",
            "Tap Water"
        )

        source_description = selected_source.get(
            "source_description",
            ""
        )

    else:

        source_type = "Tap Water"

        source_description = ""


    return (
        source_type,
        source_description
    )


# ============================================================
# LATEST LIVE READING
# ============================================================

@app.route("/latest")
def latest():

    try:

        # ----------------------------------------------------
        # Read current ESP32 values
        # ----------------------------------------------------

        sensor_data = get_sensor_data()


        ph = sensor_data["pH"]

        tds = sensor_data["TDS"]

        turbidity = sensor_data["Turbidity"]

        temperature = sensor_data["Temperature"]


        # ----------------------------------------------------
        # Decision Tree
        # ----------------------------------------------------

        model_prediction, result = make_prediction(

            ph,

            tds,

            turbidity,

            temperature

        )


        # ----------------------------------------------------
        # Get CURRENT selected water source
        # ----------------------------------------------------

        source_type, source_description = (
            get_selected_source()
        )


        # ----------------------------------------------------
        # Get previous latest reading
        # ----------------------------------------------------

        previous = db.reference(
            "latest"
        ).get()


        # ----------------------------------------------------
        # Current sensor data
        # ----------------------------------------------------

        current_sensor_values = {

            "pH":
                ph,

            "TDS":
                tds,

            "Turbidity":
                turbidity,

            "Temperature":
                temperature

        }


        # ----------------------------------------------------
        # Determine whether sensor values changed
        # ----------------------------------------------------

        values_changed = sensor_values_changed(

            previous,

            current_sensor_values

        )


        # ----------------------------------------------------
        # Create CURRENT dashboard reading
        #
        # IMPORTANT:
        # Source comes from selected_source.
        # ----------------------------------------------------

        reading = {

            "source_type":
                source_type,

            "source_description":
                source_description,

            "pH":
                ph,

            "TDS":
                tds,

            "Turbidity":
                turbidity,

            "Temperature":
                temperature,

            "classification":
                result,

            "prediction":
                result,

            "model_prediction":
                model_prediction,

            "timestamp":
                datetime.now(
                    timezone.utc
                ).isoformat(),

            "values_changed":
                values_changed

        }


        # ----------------------------------------------------
        # ALWAYS update /latest
        #
        # This is only the live dashboard value.
        # ----------------------------------------------------

        db.reference(
            "latest"
        ).set(
            reading
        )


        # ----------------------------------------------------
        # SAVE HISTORY ONLY WHEN SENSOR VALUES CHANGE
        #
        # Changing source alone does NOT create a fake
        # sensor reading.
        # ----------------------------------------------------

        if values_changed:

            history_reading = {

                "source_type":
                    source_type,

                "source_description":
                    source_description,

                "pH":
                    ph,

                "TDS":
                    tds,

                "Turbidity":
                    turbidity,

                "Temperature":
                    temperature,

                "classification":
                    result,

                "prediction":
                    result,

                "model_prediction":
                    model_prediction,

                "timestamp":
                    datetime.now(
                        timezone.utc
                    ).isoformat()

            }


            db.reference(
                "readings"
            ).push(
                history_reading
            )


            print(
                "New sensor reading saved to history:",
                history_reading
            )


        else:

            print(
                "Sensor values unchanged - "
                "history record not created."
            )


        # ----------------------------------------------------
        # Return current dashboard reading
        # ----------------------------------------------------

        return jsonify(
            reading
        )


    except Exception as e:

        print(
            "Latest data error:",
            str(e)
        )


        return jsonify({

            "error":
                str(e)

        }), 500


# ============================================================
# MANUAL PREDICTION ENDPOINT
# ============================================================

@app.route(
    "/predict",
    methods=["POST"]
)
def predict():

    try:

        data = request.get_json()


        ph = float(
            data["pH"]
        )

        tds = float(
            data["TDS"]
        )

        turbidity = float(
            data["Turbidity"]
        )

        temperature = float(
            data["Temperature"]
        )


        model_prediction, result = make_prediction(

            ph,

            tds,

            turbidity,

            temperature

        )


        return jsonify({

            "status":
                result,

            "model_prediction":
                model_prediction,

            "pH":
                ph,

            "TDS":
                tds,

            "Turbidity":
                turbidity,

            "Temperature":
                temperature

        })


    except Exception as e:

        return jsonify({

            "error":
                str(e)

        }), 400


# ============================================================
# HISTORY
# ============================================================

@app.route("/history")
def history():

    try:

        data = db.reference(
            "readings"
        ).get()


        if data is None:

            return jsonify([])


        readings = []


        for key, value in data.items():

            reading = dict(value)


            reading["id"] = key


            # ------------------------------------------------
            # Old records should KEEP their original source.
            # ------------------------------------------------

            if not reading.get(
                "source_type"
            ):

                reading["source_type"] = "Unknown"


            if reading.get(
                "source_description"
            ) is None:

                reading["source_description"] = ""


            readings.append(
                reading
            )


        # ----------------------------------------------------
        # Newest first
        # ----------------------------------------------------

        readings.sort(

            key=lambda x:
                x.get(
                    "timestamp",
                    ""
                ),

            reverse=True

        )


        return jsonify(
            readings
        )


    except Exception as e:

        print(
            "History error:",
            str(e)
        )


        return jsonify({

            "error":
                str(e)

        }), 500


# ============================================================
# RUN APPLICATION
# ============================================================

if __name__ == "__main__":

    app.run(

        host="0.0.0.0",

        port=5000,

        debug=True

    )