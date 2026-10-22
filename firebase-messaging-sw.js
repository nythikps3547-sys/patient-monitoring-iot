importScripts("https://www.gstatic.com/firebasejs/10.12.2/firebase-app-compat.js");
importScripts("https://www.gstatic.com/firebasejs/10.12.2/firebase-messaging-compat.js");

firebase.initializeApp({
  apiKey: "AIzaSyDTsZETn9adSCIfHRYwcAthN21qdA8eRr0",
  authDomain: "patient-monitoring-iot-c87f7.firebaseapp.com",
  databaseURL: "https://patient-monitoring-iot-c87f7-default-rtdb.asia-southeast1.firebasedatabase.app",
  projectId: "patient-monitoring-iot-c87f7",
  storageBucket: "patient-monitoring-iot-c87f7.firebasestorage.app",
  messagingSenderId: "1089208127153",
  appId: "1:1089208127153:web:1a3c8264dbdd240ea55c90"
});

const messaging = firebase.messaging();

messaging.onBackgroundMessage((payload) => {
  const title = payload.notification?.title || "PatientCare Alert";
  const options = {
    body: payload.notification?.body || "Please check your PatientCare dashboard."
  };

  self.registration.showNotification(title, options);
});
