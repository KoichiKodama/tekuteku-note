const ALGORITHM_NAME = 'AES-CTR';
const KEY_BIT_LENGTH = 128; 
const AES_BLOCK_SIZE_BYTES = 16; 
const AES_BLOCK_SIZE_BITS = 128;

const encoder = new TextEncoder();
const decoder = new TextDecoder();

// Uint8Array を Base64 文字列に安全に変換する
function bytesToBase64(bytes) {
	let binary = '';
	const len = bytes.byteLength;
	for (let i=0;i<len;i++) { binary += String.fromCharCode(bytes[i]); }
	return btoa(binary);
}

// Base64 文字列を Uint8Array に安全に変換する
function base64ToBytes(base64) {
	const binaryString = atob(base64);
	const len = binaryString.length;
	const bytes = new Uint8Array(len);
	for (let i=0;i<len;i++) { bytes[i] = binaryString.charCodeAt(i); }
	return bytes;
}

async function deriveKeyAndCounter(passphrase,salt) {
	const passphraseBytes = encoder.encode(passphrase);
	const baseKey = await crypto.subtle.importKey('raw',passphraseBytes,'PBKDF2',false,['deriveBits']);
	const keyByteLength = KEY_BIT_LENGTH/8;
	const totalByteLength = keyByteLength+AES_BLOCK_SIZE_BYTES; 
	const totalBitLength = totalByteLength*8;
	const derivedBits = await crypto.subtle.deriveBits({name:'PBKDF2',salt:salt,iterations:10000,hash:'SHA-256'},baseKey,totalBitLength);
	const derivedBytes = new Uint8Array(derivedBits);
	const keyBytes = derivedBytes.slice(0,keyByteLength);
	const counterBytes = derivedBytes.slice(keyByteLength,totalByteLength);
	const cryptoKey = await crypto.subtle.importKey('raw',keyBytes,ALGORITHM_NAME,false,['encrypt','decrypt']);
	return { cryptoKey,counterBytes };
}

async function encrypt(plaintext,passphrase) {
	const salt = crypto.getRandomValues(new Uint8Array(8));
	const { cryptoKey,counterBytes } = await deriveKeyAndCounter(passphrase,salt);
	const plaintextBytes = encoder.encode(plaintext);
	const encryptedBuffer = await crypto.subtle.encrypt({name:ALGORITHM_NAME,counter:counterBytes,length:AES_BLOCK_SIZE_BITS},cryptoKey,plaintextBytes);
	const encryptedBytes = new Uint8Array(encryptedBuffer);
	const magic = encoder.encode("Salted__");
	const resultBytes = new Uint8Array(magic.length+salt.length+encryptedBytes.length);
	resultBytes.set(magic,0);
	resultBytes.set(salt,magic.length);
	resultBytes.set(encryptedBytes,magic.length+salt.length);
	return bytesToBase64(resultBytes);
}

async function decrypt(base64CipherText,passphrase) {
	const encryptedBytes = base64ToBytes(base64CipherText);
	const magicLength = 8; // "Salted__" の長さ
	const saltLength = 8;  // Saltの長さ
	const salt = encryptedBytes.slice(magicLength,magicLength+saltLength);
	const pureCipherTextBytes = encryptedBytes.slice(magicLength+saltLength);
	const { cryptoKey,counterBytes } = await deriveKeyAndCounter(passphrase,salt);
	const decryptedBuffer = await crypto.subtle.decrypt({name:ALGORITHM_NAME,counter:counterBytes,length:AES_BLOCK_SIZE_BITS},cryptoKey,pureCipherTextBytes);
	return decoder.decode(decryptedBuffer);
}
