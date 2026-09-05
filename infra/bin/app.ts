import * as cdk from 'aws-cdk-lib';
import { TrainingIotStack } from '../lib/training-iot-stack';

const app = new cdk.App();

new TrainingIotStack(app, 'TrainingIotStack', {
  description: 'Training telemetry: AWS IoT Basic Ingest to Firehose, S3, and Athena.',
});
