import pandas as pd
from sklearn.model_selection import train_test_split
from sklearn.tree import DecisionTreeClassifier
from sklearn.metrics import accuracy_score, classification_report
import joblib

# Load dataset
df = pd.read_csv("water_quality_dataset1.csv")

# Features
X = df[["pH","TDS_ppm","Turbidity_NTU","Temperature_C"]]

# Target
y = df["Label"]

# Split dataset
X_train, X_test, y_train, y_test = train_test_split(
    X, y,
    test_size=0.2,
    random_state=42
)

# Decision Tree
model = DecisionTreeClassifier(random_state=42)

model.fit(X_train,y_train)

pred=model.predict(X_test)

print("Accuracy:",accuracy_score(y_test,pred))
print(classification_report(y_test,pred))

joblib.dump(model,"model.pkl")

print("Model saved successfully.")