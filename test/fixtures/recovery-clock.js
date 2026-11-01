/* Make the old 60-second recovery expiry fail promptly, without a minute-long test. */
const realNow = Date.now;
let elapsed = 0;
Date.now = () => realNow() + (elapsed += 90000);
