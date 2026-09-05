import * as cdk from 'aws-cdk-lib';
import * as athena from 'aws-cdk-lib/aws-athena';
import * as firehose from 'aws-cdk-lib/aws-kinesisfirehose';
import * as glue from 'aws-cdk-lib/aws-glue';
import * as iam from 'aws-cdk-lib/aws-iam';
import * as iot from 'aws-cdk-lib/aws-iot';
import * as logs from 'aws-cdk-lib/aws-logs';
import * as s3 from 'aws-cdk-lib/aws-s3';
import { Construct } from 'constructs';

const RULE_NAME = 'training_iot_basic_ingest';
const DATABASE_NAME = 'training_iot';
const TABLE_NAME = 'telemetry_raw';
const WORKGROUP_NAME = 'training-iot';
const DEVICE_POLICY_NAME = 'training-iot-device-telemetry';

export class TrainingIotStack extends cdk.Stack {
  constructor(scope: Construct, id: string, props?: cdk.StackProps) {
    super(scope, id, props);

    const rawBucket = new s3.Bucket(this, 'RawTelemetryBucket', {
      blockPublicAccess: s3.BlockPublicAccess.BLOCK_ALL,
      encryption: s3.BucketEncryption.S3_MANAGED,
      enforceSSL: true,
      removalPolicy: cdk.RemovalPolicy.RETAIN,
    });

    const athenaResultsBucket = new s3.Bucket(this, 'AthenaResultsBucket', {
      blockPublicAccess: s3.BlockPublicAccess.BLOCK_ALL,
      encryption: s3.BucketEncryption.S3_MANAGED,
      enforceSSL: true,
      removalPolicy: cdk.RemovalPolicy.RETAIN,
    });

    const firehoseRole = new iam.Role(this, 'FirehoseDeliveryRole', {
      assumedBy: new iam.ServicePrincipal('firehose.amazonaws.com'),
    });
    firehoseRole.addToPolicy(
      new iam.PolicyStatement({
        actions: [
          's3:AbortMultipartUpload',
          's3:GetBucketLocation',
          's3:GetObject',
          's3:ListBucket',
          's3:ListBucketMultipartUploads',
          's3:PutObject',
        ],
        resources: [rawBucket.bucketArn, rawBucket.arnForObjects('*')],
      }),
    );

    const deliveryStream = new firehose.CfnDeliveryStream(this, 'TelemetryDeliveryStream', {
      deliveryStreamType: 'DirectPut',
      extendedS3DestinationConfiguration: {
        bucketArn: rawBucket.bucketArn,
        roleArn: firehoseRole.roleArn,
        compressionFormat: 'GZIP',
        customTimeZone: 'Asia/Tokyo',
        bufferingHints: {
          intervalInSeconds: 60,
          sizeInMBs: 1,
        },
        prefix: 'raw/!{timestamp:yyyy}/!{timestamp:MM}/!{timestamp:dd}/!{timestamp:HH}/',
        errorOutputPrefix: 'errors/!{firehose:error-output-type}/!{timestamp:yyyy}/!{timestamp:MM}/!{timestamp:dd}/',
      },
    });

    const iotRuleRole = new iam.Role(this, 'IotRuleFirehoseRole', {
      assumedBy: new iam.ServicePrincipal('iot.amazonaws.com'),
    });
    iotRuleRole.addToPolicy(
      new iam.PolicyStatement({
        actions: ['firehose:PutRecord'],
        resources: [deliveryStream.attrArn],
      }),
    );

    const ruleErrorLogGroup = new logs.LogGroup(this, 'IotRuleErrorLogGroup', {
      retention: logs.RetentionDays.ONE_WEEK,
      removalPolicy: cdk.RemovalPolicy.RETAIN,
    });
    const iotRuleLogsRole = new iam.Role(this, 'IotRuleLogsRole', {
      assumedBy: new iam.ServicePrincipal('iot.amazonaws.com'),
    });
    ruleErrorLogGroup.grantWrite(iotRuleLogsRole);

    const telemetryRule = new iot.CfnTopicRule(this, 'TelemetryRule', {
      ruleName: RULE_NAME,
      topicRulePayload: {
        awsIotSqlVersion: '2016-03-23',
        // A Basic Ingest-only rule is selected by the rule name in the publish topic.
        // The event must include deviceId because Basic Ingest does not expose a subscribable telemetry topic.
        sql: 'SELECT *, timestamp() AS ingested_at',
        actions: [
          {
            firehose: {
              deliveryStreamName: deliveryStream.ref,
              roleArn: iotRuleRole.roleArn,
              separator: '\n',
            },
          },
        ],
        errorAction: {
          cloudwatchLogs: {
            logGroupName: ruleErrorLogGroup.logGroupName,
            roleArn: iotRuleLogsRole.roleArn,
          },
        },
      },
    });

    // 証明書はスタックに保存せず、実機ごとに作成して Thing に関連付ける。
    // このポリシーは、Thing 名と同じ device_id の Basic Ingest トピックだけを許可する。
    new iot.CfnPolicy(this, 'DeviceTelemetryPolicy', {
      policyName: DEVICE_POLICY_NAME,
      policyDocument: {
        Version: '2012-10-17',
        Statement: [
          {
            Effect: 'Allow',
            Action: 'iot:Connect',
            Resource: `arn:${cdk.Aws.PARTITION}:iot:${cdk.Aws.REGION}:${cdk.Aws.ACCOUNT_ID}:client/\${iot:Connection.Thing.ThingName}`,
            Condition: {
              Bool: { 'iot:Connection.Thing.IsAttached': 'true' },
            },
          },
          {
            Effect: 'Allow',
            Action: 'iot:Publish',
            Resource: `arn:${cdk.Aws.PARTITION}:iot:${cdk.Aws.REGION}:${cdk.Aws.ACCOUNT_ID}:topic/$aws/rules/${RULE_NAME}/training/\${iot:Connection.Thing.ThingName}/telemetry`,
          },
        ],
      },
    });
    const database = new glue.CfnDatabase(this, 'TelemetryDatabase', {
      catalogId: cdk.Aws.ACCOUNT_ID,
      databaseInput: { name: DATABASE_NAME },
    });

    new glue.CfnTable(this, 'TelemetryRawTable', {
      catalogId: cdk.Aws.ACCOUNT_ID,
      databaseName: database.ref,
      tableInput: {
        name: TABLE_NAME,
        tableType: 'EXTERNAL_TABLE',
        parameters: {
          classification: 'json',
          'projection.enabled': 'true',
          'projection.datehour.type': 'date',
          'projection.datehour.format': 'yyyy/MM/dd/HH',
          // Firehoseの保存先はJST。AthenaのNOWはUTC基準のため、同時刻帯が
          // 投影対象外にならないよう将来日付まで明示する。
          'projection.datehour.range': '2026/01/01/00,2030/12/31/23',
          'projection.datehour.interval': '1',
          'projection.datehour.interval.unit': 'HOURS',
          'storage.location.template': `${rawBucket.s3UrlForObject('raw/')}${'${datehour}'}/`,
        },
        partitionKeys: [{ name: 'datehour', type: 'string' }],
        storageDescriptor: {
          columns: [
            { name: 'schema_version', type: 'int' },
            { name: 'device_id', type: 'string' },
            { name: 'session_id', type: 'string' },
            { name: 'captured_at', type: 'string' },
            { name: 'ingested_at', type: 'bigint' },
            { name: 'sequence', type: 'bigint' },
            { name: 'sampling_hz', type: 'int' },
            {
              name: 'samples',
              type: 'array<struct<offset_ms:int,ax:double,ay:double,az:double,gx:double,gy:double,gz:double>>',
            },
          ],
          location: rawBucket.s3UrlForObject('raw/'),
          inputFormat: 'org.apache.hadoop.mapred.TextInputFormat',
          outputFormat: 'org.apache.hadoop.hive.ql.io.HiveIgnoreKeyTextOutputFormat',
          serdeInfo: {
            serializationLibrary: 'org.openx.data.jsonserde.JsonSerDe',
            parameters: { 'ignore.malformed.json': 'true' },
          },
        },
      },
    });

    new athena.CfnWorkGroup(this, 'AthenaWorkGroup', {
      name: WORKGROUP_NAME,
      state: 'ENABLED',
      workGroupConfiguration: {
        enforceWorkGroupConfiguration: true,
        publishCloudWatchMetricsEnabled: true,
        bytesScannedCutoffPerQuery: 104857600,
        resultConfiguration: {
          outputLocation: athenaResultsBucket.s3UrlForObject('athena-results/'),
          encryptionConfiguration: { encryptionOption: 'SSE_S3' },
        },
      },
    });

    new cdk.CfnOutput(this, 'BasicIngestTopic', {
      value: `$aws/rules/${RULE_NAME}/training/{deviceId}/telemetry`,
      description: 'Publish batched telemetry to this topic. It cannot be subscribed to.',
    });
    new cdk.CfnOutput(this, 'RawTelemetryBucketName', { value: rawBucket.bucketName });
    new cdk.CfnOutput(this, 'FirehoseDeliveryStreamName', { value: deliveryStream.ref });
    new cdk.CfnOutput(this, 'AthenaResultsBucketName', { value: athenaResultsBucket.bucketName });
    new cdk.CfnOutput(this, 'AthenaDatabaseName', { value: DATABASE_NAME });
    new cdk.CfnOutput(this, 'AthenaTableName', { value: TABLE_NAME });
    new cdk.CfnOutput(this, 'AthenaWorkgroupName', { value: WORKGROUP_NAME });
    new cdk.CfnOutput(this, 'DeviceTelemetryPolicyName', { value: DEVICE_POLICY_NAME });
  }
}
