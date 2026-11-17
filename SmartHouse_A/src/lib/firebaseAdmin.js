import admin from "firebase-admin";

// Server đọc/ghi Firebase theo một trong hai cách, chọn theo biến môi trường (.env, hoặc Settings →
// Environment Variables trên Vercel). Tài khoản chỉ nằm trên server, trang web không thấy.
//   1. FIREBASE_SERVICE_ACCOUNT: khóa service account (JSON trên một dòng), dùng firebase-admin.
//   2. FIREBASE_EMAIL + FIREBASE_PASSWORD: tài khoản Firebase Authentication có UID trong Rules
//      (vd. tài khoản của các mạch). Đăng nhập và đọc/ghi Realtime Database qua REST API.
// Khởi tạo lúc gọi lần đầu, để "npm run build" không cần các biến này.

// Cấu hình công khai của project smart-house-fcd4f (giống trong firmware và trang web)
const API_KEY = process.env.FIREBASE_API_KEY || "AIzaSyBom2FOJvMisw2NcE89Zkr-rP_dgOSqbMY";
const DATABASE_URL = (process.env.FIREBASE_DATABASE_URL || "https://smart-house-fcd4f-default-rtdb.asia-southeast1.firebasedatabase.app").replace(/\/+$/, "");

const fail = (message, status = 502) => Object.assign(new Error(message), { status });

// ==== Cách 1: service account ====
let database = null;

function adminDb() {
  if (database) return database;
  let sa;
  try {
    sa = JSON.parse(process.env.FIREBASE_SERVICE_ACCOUNT);
  } catch {
    throw new Error("FIREBASE_SERVICE_ACCOUNT trong .env không phải JSON hợp lệ (phải nằm trên một dòng).");
  }
  if (!sa.project_id) throw new Error("FIREBASE_SERVICE_ACCOUNT thiếu project_id.");
  if (!admin.apps.length) {
    admin.initializeApp({ credential: admin.credential.cert(sa), databaseURL: sa.databaseURL || DATABASE_URL });
  }
  database = admin.database();
  return database;
}

/**
 * Đăng nhập Firebase Authentication bằng email/mật khẩu qua REST. Trả về { idToken, expiresIn, email }.
 * Lỗi mang status: 401 sai tài khoản, 429 sai quá nhiều lần, 502 lỗi khác; code là mã lỗi của Firebase.
 */
export async function signInWithPassword(email, password) {
  const r = await fetch(`https://identitytoolkit.googleapis.com/v1/accounts:signInWithPassword?key=${API_KEY}`, {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({ email, password, returnSecureToken: true }),
    cache: "no-store",
  });
  const j = await r.json().catch(() => null);
  if (!r.ok || !j || !j.idToken) {
    const code = (j && j.error && j.error.message) || "HTTP " + r.status;
    const status = /TOO_MANY_ATTEMPTS/.test(code) ? 429 : /INVALID_LOGIN|INVALID_PASSWORD|EMAIL_NOT_FOUND|INVALID_EMAIL|USER_DISABLED/.test(code) ? 401 : 502;
    throw Object.assign(fail(code, status), { code });
  }
  return j;
}

// ==== Cách 2: email + mật khẩu ====
let session = null;  // { idToken, expiresAt }

async function idToken() {
  if (session && Date.now() < session.expiresAt - 60000) return session.idToken;
  let j;
  try {
    j = await signInWithPassword(process.env.FIREBASE_EMAIL, process.env.FIREBASE_PASSWORD);
  } catch (e) {
    throw fail(e.status === 401 ? "Sai FIREBASE_EMAIL hoặc FIREBASE_PASSWORD trong biến môi trường." : "Không đăng nhập được Firebase: " + e.message);
  }
  session = { idToken: j.idToken, expiresAt: Date.now() + Number(j.expiresIn || 3600) * 1000 };
  return session.idToken;
}

async function rest(method, path, body) {
  // /home.json, hoặc /.json cho gốc (ghi nhiều đường dẫn một lần)
  const url = `${DATABASE_URL}/${path}.json?auth=${encodeURIComponent(await idToken())}`;
  const r = await fetch(url, {
    method,
    headers: body ? { "Content-Type": "application/json" } : undefined,
    body: body ? JSON.stringify(body) : undefined,
    cache: "no-store",
  });
  const j = await r.json().catch(() => null);
  if (r.status === 401) {
    session = null;  // token hết hạn hoặc bị thu hồi: lần sau đăng nhập lại
    throw fail("Firebase từ chối: tài khoản FIREBASE_EMAIL không có quyền với /home (kiểm tra UID trong Rules).");
  }
  if (!r.ok) throw fail("Firebase trả về lỗi: " + ((j && j.error) || "HTTP " + r.status));
  return j;
}

// Giống phần firebase-admin mà API dùng: ref(path).once("value") và ref().update({...})
const restRef = (path) => ({
  once: async () => {
    const v = await rest("GET", path || "");
    return { val: () => v };
  },
  update: (values) => rest("PATCH", path || "", values),
});

function pick() {
  if (process.env.FIREBASE_SERVICE_ACCOUNT) return "admin";
  if (process.env.FIREBASE_EMAIL && process.env.FIREBASE_PASSWORD) return "rest";
  throw fail("Chưa cấu hình Firebase cho server: đặt FIREBASE_EMAIL và FIREBASE_PASSWORD (hoặc FIREBASE_SERVICE_ACCOUNT) trong .env hoặc trên Vercel.", 500);
}

// Dùng như firebase-admin: db.ref("home").once("value"), db.ref().update({...})
export const db = { ref: (path) => (pick() === "admin" ? adminDb().ref(path) : restRef(path)) };

/** Chạy một lệnh Firebase, quá 10 giây thì báo lỗi thay vì treo. */
export function withTimeout(promise, ms = 10000) {
  let t;
  const timer = new Promise((_, rej) => { t = setTimeout(() => rej(Object.assign(new Error("Firebase không phản hồi sau " + ms / 1000 + " giây. Kiểm tra Internet và cấu hình Firebase trong biến môi trường."), { status: 504 })), ms); });
  return Promise.race([promise, timer]).finally(() => clearTimeout(t));
}
export default admin;
