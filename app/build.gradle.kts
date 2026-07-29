import java.util.Properties

plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}

val localProperties = Properties()
val localPropertiesFile = rootProject.file("local.properties")
if (localPropertiesFile.isFile) {
    localPropertiesFile.inputStream().use(localProperties::load)
}
val testCallerE164 =
    providers.gradleProperty("ivrdroid.testCallerE164").orNull
        ?: localProperties.getProperty("ivrdroid.testCallerE164", "")
require(testCallerE164.isEmpty() || testCallerE164.matches(Regex("""\+[0-9]{8,15}"""))) {
    "ivrdroid.testCallerE164 must be empty or an E.164 number such as +15551234567."
}

android {
    namespace = "ai.rx1.ivrdroid"
    compileSdk = 35

    defaultConfig {
        applicationId = "ai.rx1.ivrdroid"
        minSdk = 29
        targetSdk = 35
        versionCode = 3
        versionName = "0.2.1-dev"

        buildConfigField("String", "TEST_CALLER_E164", "\"$testCallerE164\"")
        testInstrumentationRunner = "android.app.Instrumentation"
    }

    buildTypes {
        release {
            isMinifyEnabled = true
            proguardFiles(
                getDefaultProguardFile("proguard-android-optimize.txt"),
                "proguard-rules.pro",
            )
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    kotlinOptions {
        jvmTarget = "17"
    }

    buildFeatures {
        buildConfig = true
    }

    testOptions {
        unitTests.isReturnDefaultValues = true
    }
}

dependencies {
    testImplementation("junit:junit:4.13.2")
    testImplementation("org.json:json:20240303")
}
