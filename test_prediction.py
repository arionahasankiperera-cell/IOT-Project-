import requests


url = "http://127.0.0.1:5000/predict"


data = {

    "source_type":
        "Other",

    "source_description":
        "River water",

    "pH":
        7.38,

    "TDS":
        138.7,

    "Turbidity":
        0.25,

    "Temperature":
        20.25

}


response = requests.post(
    url,
    json=data
)


print(
    "Status code:",
    response.status_code
)


print(
    "Response:"
)


print(
    response.json()
)