importScripts("https://www.gstatic.com/firebasejs/10.12.2/firebase-app-compat.js");
importScripts("https://www.gstatic.com/firebasejs/10.12.2/firebase-messaging-compat.js");

// Firebase configuration
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

// Background notification handler
messaging.onBackgroundMessage((payload) => {
  const data = payload.data || {};

  // Avoid manually displaying a duplicate notification
  // when the message already contains a notification payload.
  if (payload.notification) {
    return;
  }

  const title = data.title || "PatientCare Alert";

  const options = {
    body: data.body || "Please check your PatientCare dashboard.",
    icon: "./icon.png",
    badge: "./icon.png",
    tag: data.tag || "patientcare-alert",
    renotify: false,
    data: {
      url: "./"
    }
  };

  return self.registration.showNotification(title, options);
});

// Open dashboard when notification is clicked
self.addEventListener("notificationclick", (event) => {
  event.notification.close();

  const targetUrl = new URL("./", self.location.origin).href;

  event.waitUntil(
    clients.matchAll({
      type: "window",
      includeUncontrolled: true
    }).then((clientList) => {
      for (const client of clientList) {
        if (client.url.startsWith(self.location.origin)) {
          return client.focus();
        }
      }

      return clients.openWindow(targetUrl);
    })
  );
});
